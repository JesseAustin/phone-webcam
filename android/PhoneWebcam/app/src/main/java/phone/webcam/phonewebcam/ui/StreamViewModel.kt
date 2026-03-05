package phone.webcam.phonewebcam.ui

import android.Manifest
import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.IBinder
import android.util.Log
import android.view.Surface
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import phone.webcam.phonewebcam.camera.CameraManager
import phone.webcam.phonewebcam.network.HandshakeServer
import phone.webcam.phonewebcam.network.NetworkDiscovery
import phone.webcam.phonewebcam.security.PasswordManager
import phone.webcam.phonewebcam.service.CameraStreamService
import java.net.InetAddress
import kotlin.coroutines.cancellation.CancellationException

class StreamViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "StreamViewModel"
        private const val DEFAULT_IP   = "192.168.1.100"
        private const val DEFAULT_PORT = 9000
    }

    private var userWantsToStream = false
    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()
    private var handshakeJob: Job? = null
    private var streamService: CameraStreamService? = null
    private var isBound = false
    private var _pendingPreviewSurface: Surface? = null
    private var serviceHasConfirmedStreaming = false
    private var streamWasStoppedByUser = true
    private var lastHandshakeIp: String? = null
    private var lastHandshakePort: Int = 9000

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as CameraStreamService.LocalBinder
            streamService = binder.getService()
            isBound = true

            // The SurfaceView's surfaceCreated fires before the service is bound, so
            // setPreviewSurface() was a no-op. Flush the pending surface now.
            // setPreviewSurface() calls attachPreviewSurface() if already streaming,
            // or just stores it so startCapture() picks it up if the camera isn't open yet.
            _pendingPreviewSurface?.let { surface ->
                streamService?.setPreviewSurface(surface)
            }

            lastHandshakeIp?.let { ip ->
                streamService?.setRtpDestination(ip, lastHandshakePort)
            }
            startStatusPolling()
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            streamService = null
            isBound = false
        }
    }

    fun toggleCamera() {
        val newIsFront = !_uiState.value.isFrontCamera
        _uiState.update { it.copy(isFrontCamera = newIsFront) }
        streamService?.switchCamera(newIsFront)
    }

    fun toggleMic() {
        val newMicEnabled = !_uiState.value.isMicEnabled
        _uiState.update { it.copy(isMicEnabled = newMicEnabled) }
        streamService?.setMicEnabled(newMicEnabled)
    }

    private val handshakeServer = HandshakeServer { ip, port ->
        Log.i(TAG, "Handshake: received from OBS — ip=$ip port=$port")

        handshakeJob?.cancel()
        handshakeJob = viewModelScope.launch(Dispatchers.Main) {
            try {
                val state = _uiState.value

                if (!state.autoDiscoveryEnabled && state.targetPort != port) {
                    Log.w(TAG, "Port mismatch! App expects ${state.targetPort}, OBS is on $port. Ignoring.")
                    _uiState.update { it.copy(statusMessage = "Port Mismatch (OBS is on $port)") }
                    return@launch
                }

                if (!state.isStreaming && state.targetPort != port) {
                    if (state.autoDiscoveryEnabled) {
                        _uiState.update { it.copy(
                            targetIp = ip,
                            targetPort = port,
                            portText = port.toString()
                        )}
                    }
                }

                val isCurrentDestination = state.isStreaming &&
                        state.targetIp == ip && state.targetPort == port
                if (isCurrentDestination) {
                    Log.i(TAG, "Handshake received for existing stream - forcing SRTP reset via redirect")
                    streamService?.redirectStream(ip, port)
                    return@launch
                }

                val newState = state.copy(
                    targetIp = ip,
                    targetPort = if (state.autoDiscoveryEnabled) port else state.targetPort,
                    statusMessage = "Linked with OBS at $ip"
                )
                _uiState.value = newState

                if (state.isStreaming) {
                    streamService?.redirectStream(ip, port)
                } else {
                    if (userWantsToStream) {
                        Log.i(TAG, "Handshake verified — launching stream to $ip:$port")
                        launchStream(newState)
                    } else {
                        Log.i(TAG, "Handshake received but user has not pressed start — ignoring")
                        _uiState.update { it.copy(statusMessage = "OBS linked — press Start to stream") }
                    }
                }

                savePreferences()
            } catch (e: CancellationException) {
                Log.i(TAG, "Handshake superseded by newer request from OBS")
            }
        }
    }

    fun setPreviewSurface(surface: Surface?) {
        _pendingPreviewSurface = surface
        // Always forward to service — it will attach or detach as appropriate
        streamService?.setPreviewSurface(surface)
    }

    private val networkDiscovery = NetworkDiscovery(application)

    init {
        handshakeServer.start()
        networkDiscovery.startAdvertising(9000)
        loadPreferences()
    }

    private fun hasCameraPermission(): Boolean =
        ContextCompat.checkSelfPermission(getApplication(), Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED

    fun updateIp(ip: String) {
        _uiState.value = _uiState.value.copy(targetIp = ip)
        savePreferences()

        if (!uiState.value.autoDiscoveryEnabled && isValidIp(ip)) {
            viewModelScope.launch(Dispatchers.IO) {
                initiateHandshake(ip, _uiState.value.targetPort)
            }
        }

        if (ip.isBlank()) {
            networkDiscovery.stopAdvertising()
            networkDiscovery.startAdvertising(_uiState.value.targetPort)
        }
    }

    private fun initiateHandshake(ip: String, port: Int) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val socket = java.net.Socket()
                socket.connect(java.net.InetSocketAddress(ip, HandshakeServer.HANDSHAKE_PORT), 2000)
                val writer = socket.getOutputStream().bufferedWriter()

                val currentPassword = _uiState.value.password
                Log.d("PhoneWebcam", "onStartCommand: password empty=${currentPassword.isEmpty()}")

                val ts = System.currentTimeMillis() / 1000

                val json = if (currentPassword.isNotEmpty()) {
                    // Generate a random 32-byte challenge and sign it with our password.
                    // OBS receives this, recomputes the HMAC, and rejects if it does not match.
                    val challenge = ByteArray(32).also { java.security.SecureRandom().nextBytes(it) }
                    val challengeHex = challenge.joinToString("") { "%02x".format(it) }
                    val hmac = phone.webcam.phonewebcam.security.HmacAuth.computeHmac(currentPassword, challenge)
                    Log.d(TAG, "HMAC Debug - Pass: $currentPassword, Challenge: $challengeHex, Result: $hmac")

                    """{"request":"handshake","challenge":"$challengeHex","hmac":"$hmac","ts":$ts}"""
                }
                else {
                    """{"request":"handshake","ts":$ts}"""
                }

                writer.write(json)
                writer.newLine()
                writer.flush()
                socket.close()
                Log.i(TAG, "Handshake request sent to OBS at $ip (auth: ${if (currentPassword.isNotEmpty()) "HMAC" else "none"})")
            } catch (e: Exception) {
                Log.w(TAG, "Handshake request failed: ${e.message}")
            }
        }
    }

    fun updateBitrate(mbps: Int) {
        _uiState.update { it.copy(bitrateMbps = mbps) }
        savePreferences()
    }

    private fun isValidIp(ip: String): Boolean {
        return try {
            InetAddress.getByName(ip)
            ip.isNotBlank()
        } catch (e: Exception) { false }
    }

    fun updatePort(input: String) {
        _uiState.update { it.copy(portText = input) }

        val numericPort = input.toIntOrNull()
        if (numericPort != null && numericPort in 1..65535) {
            _uiState.update { it.copy(
                targetPort = numericPort,
                autoDiscoveryEnabled = false
            ) }
            // Use the master save function to avoid key conflicts
            savePreferences()

            if (_uiState.value.isStreaming) {
                stopStreaming()
            }
        }
    }

    fun finalizePort() {
        val state = _uiState.value
        val numericPort = state.portText.toIntOrNull()

        if (numericPort == null || numericPort !in 1..65535) {
            // Reset to default 9000 if invalid/blank when clicking away
            val defaultPort = 9000
            _uiState.update { it.copy(portText = defaultPort.toString(), targetPort = defaultPort) }
            savePreferences()
        }
    }

    fun updateResolution(resolution: CameraManager.StreamResolution) {
        _uiState.update { it.copy(selectedResolution = resolution) }
        savePreferences()
    }

    // -----------------------------------------------------------------------
    // Password management
    //
    // Live update — same pattern as port:
    //   updatePassword() → saves to EncryptedSharedPrefs → pushes to
    //   handshakeServer.password so the next incoming OBS handshake uses
    //   the new value immediately.
    //
    // No button, no restart required. If a stream is active when the password
    // changes, OBS will also have restarted its stream (via its own update
    // callback), causing a new handshake that will be validated with the new
    // password on both ends.
    // -----------------------------------------------------------------------
    fun updatePassword(password: String) {
        PasswordManager.setPassword(getApplication(), password)
        handshakeServer.password = password      // @Volatile — safe cross-thread
        _uiState.update { it.copy(password = password) }
        // Note: we intentionally do NOT save the password in the normal
        // stream_prefs — it lives only in EncryptedSharedPreferences.
        Log.i(TAG, "Password updated (auth: ${if (password.isEmpty()) "disabled" else "enabled"})")
    }

    fun startStreaming() {
        userWantsToStream = true
        serviceHasConfirmedStreaming = false
        val state = _uiState.value
        if (state.isStreaming) return

        if (state.targetIp.isBlank()) return

        // Always initiate handshake first.
        // Only call launchStream() inside the handshakeServer success callback.
        _uiState.update { it.copy(statusMessage = "Authenticating with OBS...") }
        initiateHandshake(state.targetIp, state.targetPort)
    }

    private fun launchStream(state: StreamUiState) {
        val context = getApplication<Application>().applicationContext
        val intent = Intent(context, CameraStreamService::class.java).apply {
            action = CameraStreamService.ACTION_START_STREAM
            putExtra(CameraStreamService.EXTRA_TARGET_IP, state.targetIp)
            putExtra(CameraStreamService.EXTRA_TARGET_PORT, state.targetPort)
            putExtra(CameraStreamService.EXTRA_BITRATE_MBPS, state.bitrateMbps)
            putExtra(CameraStreamService.EXTRA_RESOLUTION, state.selectedResolution.name)
            putExtra(CameraStreamService.EXTRA_PASSWORD, state.password)
        }

        if (streamService == null) {
            context.startForegroundService(intent)
        }

        context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)

        _uiState.value = state.copy(isStreaming = true, statusMessage = "Starting stream...")
        Log.i(TAG, "Stream started: ${state.targetIp}:${state.targetPort}")
    }

    fun stopStreaming() {
        userWantsToStream = false

        val context = getApplication<Application>().applicationContext
        streamWasStoppedByUser = true

        val intent = Intent(context, CameraStreamService::class.java).apply {
            action = CameraStreamService.ACTION_STOP_STREAM
        }
        context.startService(intent)

        if (isBound) {
            context.unbindService(serviceConnection)
            isBound = false
        }

        streamService = null
        _uiState.value = _uiState.value.copy(
            isStreaming = false,
            statusMessage = "Stream stopped",
            frameCount = 0, fps = 0, bandwidth = 0.0
        )
    }

    private fun startStatusPolling() {
        viewModelScope.launch {
            while (isActive && isBound) {
                val service = streamService
                if (service != null) {
                    val status = service.getStreamingStatus()
                    val bandwidthMbps = if (status.elapsedSeconds > 0)
                        (status.bytesSent * 8.0 / 1_000_000) / status.elapsedSeconds else 0.0

                    _uiState.update { current ->
                        if (status.isStreaming) serviceHasConfirmedStreaming = true

                        val isUnexpectedStop = serviceHasConfirmedStreaming &&
                                current.isStreaming && !status.isStreaming

                        if (isUnexpectedStop) {
                            serviceHasConfirmedStreaming = false
                            networkDiscovery.restartAdvertising(_uiState.value.targetPort)
                            current.copy(isStreaming = false, statusMessage = "Reconnecting...")
                        } else {
                            current.copy(
                                frameCount = status.frameCount,
                                fps = status.fps.toInt(),
                                bandwidth = bandwidthMbps,
                                statusMessage = if (current.isStreaming)
                                    "Streaming to ${status.targetInfo}" else current.statusMessage
                            )
                        }
                    }
                }
                delay(100)
            }
        }
    }

    fun toggleDiscoveryMode() {
        val newAutoState = !_uiState.value.autoDiscoveryEnabled
        _uiState.update { it.copy(autoDiscoveryEnabled = newAutoState) }
        if (newAutoState) {
            networkDiscovery.stopAdvertising()
            networkDiscovery.startAdvertising(_uiState.value.targetPort)
        }
        savePreferences()
    }

    private fun loadPreferences() {
        val context = getApplication<Application>().applicationContext
        val prefs = context.getSharedPreferences("stream_prefs", Context.MODE_PRIVATE)

        val port = prefs.getInt("target_port", 9000)

        val resolutionName = prefs.getString("resolution", "RES_1080P") ?: "RES_1080P"
        val resolution = try {
            CameraManager.StreamResolution.valueOf(resolutionName)
        } catch (e: IllegalArgumentException) {
            CameraManager.StreamResolution.RES_1080P
        }

        // Load password from encrypted prefs (never from stream_prefs)
        val savedPassword = PasswordManager.getPassword(context)
        handshakeServer.password = savedPassword  // apply on restore

        val autoDiscovery = prefs.getBoolean("auto_discovery", true)
        _uiState.update { it.copy(
            targetIp             = prefs.getString("target_ip", DEFAULT_IP) ?: DEFAULT_IP,
            targetPort           = prefs.getInt("target_port", DEFAULT_PORT),
            portText             = port.toString(),
            selectedResolution   = resolution,
            bitrateMbps          = prefs.getInt("bitrate_mbps", 20),
            autoDiscoveryEnabled = autoDiscovery,
            password = savedPassword,
        ) }

    }

    private fun savePreferences() {
        val context = getApplication<Application>().applicationContext
        val prefs = context.getSharedPreferences("stream_prefs", Context.MODE_PRIVATE)
        val state = _uiState.value

        prefs.edit().apply {
            putString("target_ip", state.targetIp)
            putString("resolution", state.selectedResolution.name)
            putInt("target_port", state.targetPort)
            putInt("bitrate_mbps", state.bitrateMbps)
            putBoolean("auto_discovery", state.autoDiscoveryEnabled)
            // password intentionally excluded — stored in EncryptedSharedPreferences only
            apply()
        }
    }

    override fun onCleared() {
        handshakeServer.stop()
        networkDiscovery.stopAdvertising()
        //stopStreaming()
        //streamService?.setPreviewSurface(null)
        super.onCleared()
    }
}

data class StreamUiState(
    val isStreaming: Boolean = false,
    val targetIp: String = "192.168.1.100",
    val targetPort: Int = 9000,
    val portText: String = "9000",
    val selectedResolution: CameraManager.StreamResolution = CameraManager.StreamResolution.RES_1080P,
    val frameCount: Long = 0,
    val fps: Int = 0,
    val bandwidth: Double = 0.0,
    val bitrateMbps: Int = 20,
    val statusMessage: String = "Ready to stream",
    val autoDiscoveryEnabled: Boolean = true,
    val isFrontCamera: Boolean = false,
    val isMicEnabled: Boolean = true,
    // Password is in state so the UI field can be bound to it,
    // but it is persisted separately in EncryptedSharedPreferences.
    val password: String = ""
)