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
    private var pollingJob: Job? = null
    private var savedResolutionW: Int = 1920
    private var savedResolutionH: Int = 1080
    private val resolutionQueryManager = CameraManager(application)
    private var streamService: CameraStreamService? = null
    private var isBound = false
    private var _pendingPreviewSurface: Surface? = null
    private var serviceHasConfirmedStreaming = false
    private var streamWasStoppedByUser = true
    private var lastHandshakeIp: String? = null
    private var lastHandshakePort: Int = 9000
    private var currentSrtpSaltHex: String = ""

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as CameraStreamService.LocalBinder
            streamService = binder.getService()
            isBound = true

            _pendingPreviewSurface?.let { surface ->
                streamService?.setPreviewSurface(surface)
            }

            lastHandshakeIp?.let { ip ->
                streamService?.setRtpDestination(ip, lastHandshakePort)
            }

            startStatusPolling()
        }
        override fun onServiceDisconnected(name: ComponentName?) {

            Log.i("StreamViewModel", "onServiceDisconnected fired — isBound=$isBound userWantsToStream=$userWantsToStream")

            // Stop the polling job so it doesn't keep trying to talk to a dead service
            pollingJob?.cancel()
            pollingJob = null

            // If the service dies/stops, the UI must reflect that streaming is over
            _uiState.update { it.copy(
                isStreaming = false,
                statusMessage = "Stream stopped",
                frameCount = 0, fps = 0, bandwidth = 0.0
            ) }

            streamService = null
            isBound = false
        }
    }

    fun toggleCamera() {
        val newIsFront = !_uiState.value.isFrontCamera
        _uiState.update { it.copy(isFrontCamera = newIsFront) }
        streamService?.switchCamera(newIsFront)
        refreshSupportedResolutions(newIsFront)
        savePreferences()
    }

    fun toggleMic() {
        val newMicEnabled = !_uiState.value.isMicEnabled
        _uiState.update { it.copy(isMicEnabled = newMicEnabled) }
        streamService?.setMicEnabled(newMicEnabled)
    }

    private fun refreshSupportedResolutions(useFrontCamera: Boolean) {
        val supported = resolutionQueryManager.getSupportedResolutions(useFrontCamera)
        val current = _uiState.value.selectedResolution
        val validResolution = supported.firstOrNull { it.width == current.width && it.height == current.height }
            ?: supported.firstOrNull { it.width == savedResolutionW && it.height == savedResolutionH }
            ?: supported.firstOrNull { it.width == 1920 && it.height == 1080 }
            ?: supported.firstOrNull()
            ?: CameraManager.ResolutionOption("1080p", 1920, 1080, CameraManager.StreamResolution.RES_1080P)
        _uiState.update { it.copy(supportedResolutions = supported, selectedResolution = validResolution) }
        savePreferences()
    }
    /*
    // Drop to 15fps when minimized to avoid queue backlog
    fun minimizeStream() {
        streamService?.dropTo15Fps()
    }

    // Return to 30fps when brought back to foreground
    fun resumeStream() {
        streamService?.returnTo30Fps()
    }
    */

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
                val ts = System.currentTimeMillis() / 1000

                // Generate a fresh random salt for this session
                val saltBytes = ByteArray(32).also { java.security.SecureRandom().nextBytes(it) }
                val saltHex = saltBytes.joinToString("") { "%02x".format(it) }
                currentSrtpSaltHex = saltHex  // store for launchStream()

                val json = if (currentPassword.isNotEmpty()) {
                    val challenge = ByteArray(32).also { java.security.SecureRandom().nextBytes(it) }
                    val challengeHex = challenge.joinToString("") { "%02x".format(it) }
                    val hmac = phone.webcam.phonewebcam.security.HmacAuth.computeHmac(currentPassword, challenge)
                    """{"request":"handshake","challenge":"$challengeHex","hmac":"$hmac","ts":$ts,"salt":"$saltHex"}"""
                } else {
                    """{"request":"handshake","ts":$ts,"salt":"$saltHex"}"""
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
            val defaultPort = 9000
            _uiState.update { it.copy(portText = defaultPort.toString(), targetPort = defaultPort) }
            savePreferences()
        }
    }

    fun updateResolution(resolution: CameraManager.ResolutionOption) {
        _uiState.update { it.copy(selectedResolution = resolution) }
        savePreferences()
    }

    fun updatePassword(password: String) {
        PasswordManager.setPassword(getApplication(), password)
        handshakeServer.password = password
        _uiState.update { it.copy(password = password) }
        Log.i(TAG, "Password updated (auth: ${if (password.isEmpty()) "disabled" else "enabled"})")
    }

    fun startStreaming() {
        userWantsToStream = true
        serviceHasConfirmedStreaming = false
        val state = _uiState.value
        if (state.isStreaming) return
        if (state.targetIp.isBlank()) return

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
            putExtra(CameraStreamService.EXTRA_RESOLUTION,
                "${state.selectedResolution.width}x${state.selectedResolution.height}")
            putExtra(CameraStreamService.EXTRA_PASSWORD, state.password)
            putExtra(CameraStreamService.EXTRA_IS_FRONT_CAMERA, state.isFrontCamera)
            putExtra(CameraStreamService.EXTRA_SRTP_SALT, currentSrtpSaltHex)
        }

        if (streamService == null) {
            context.startForegroundService(intent)
        }

        context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)

        // Optimistically show starting — polling will confirm isStreaming=true
        _uiState.value = state.copy(statusMessage = "Starting stream...")
        Log.i(TAG, "Stream launching to: ${state.targetIp}:${state.targetPort}")
    }

    fun stopStreaming() {
        // Cancel polling FIRST — prevents any further state updates from polling
        pollingJob?.cancel()
        pollingJob = null

        userWantsToStream = false
        streamWasStoppedByUser = true
        serviceHasConfirmedStreaming = false

        val context = getApplication<Application>().applicationContext
        val service = streamService
        if (service != null) {
            service.stopStreaming()
        } else {
            val intent = Intent(context, CameraStreamService::class.java).apply {
                action = CameraStreamService.ACTION_STOP_STREAM
            }
            context.startService(intent)
        }

        Log.i(TAG, "stopStreaming: isBound=$isBound streamService=${streamService != null}")
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
        pollingJob?.cancel()
        pollingJob = viewModelScope.launch {
            while (isActive) {
                val service = streamService
                if (service == null || !isBound) break

                val status = service.getStreamingStatus()
                val bandwidthMbps = if (status.elapsedSeconds > 0)
                    (status.bytesSent * 8.0 / 1_000_000) / status.elapsedSeconds else 0.0

                var wasUnexpectedStop = false

                _uiState.update { current ->
                    if (status.isStreaming) serviceHasConfirmedStreaming = true

                    val isUnexpectedStop = serviceHasConfirmedStreaming &&
                            current.isStreaming && !status.isStreaming

                    if (isUnexpectedStop) {
                        serviceHasConfirmedStreaming = false
                        wasUnexpectedStop = true
                        current.copy(isStreaming = false, statusMessage = "Stream stopped")
                    } else {
                        current.copy(
                            isStreaming = if (status.isStreaming) true else current.isStreaming,
                            frameCount = status.frameCount,
                            fps = status.fps.toInt(),
                            bandwidth = bandwidthMbps,
                            statusMessage = if (status.isStreaming)
                                "Streaming to ${status.targetInfo}" else current.statusMessage
                        )
                    }
                }

                if (wasUnexpectedStop) {
                    stopStreaming()
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

        val savedPassword = PasswordManager.getPassword(context)
        handshakeServer.password = savedPassword

        val autoDiscovery = prefs.getBoolean("auto_discovery", true)
        val isFront = prefs.getBoolean("is_front_camera", false)

        //val isFront = prefs.getBoolean("is_front_camera", false)
        //val supported = resolutionQueryManager.getSupportedResolutions(isFront)
        //val validResolution = if (resolution in supported) resolution
        //else supported.lastOrNull() ?: CameraManager.StreamResolution.RES_1080P

        // match saved WxH against supported list after refresh:
        val resString = prefs.getString("resolution", "1920x1080") ?: "1920x1080"
        val parts = resString.split("x")
        val savedW = parts.getOrNull(0)?.toIntOrNull() ?: 1920
        val savedH = parts.getOrNull(1)?.toIntOrNull() ?: 1080
        // Store for matching after refreshSupportedResolutions() runs:
        savedResolutionW = savedW
        savedResolutionH = savedH



        _uiState.update { it.copy(
            targetIp             = prefs.getString("target_ip", DEFAULT_IP) ?: DEFAULT_IP,
            targetPort           = prefs.getInt("target_port", DEFAULT_PORT),
            portText             = port.toString(),
            bitrateMbps          = prefs.getInt("bitrate_mbps", 20),
            autoDiscoveryEnabled = autoDiscovery,
            password             = savedPassword,
            isFrontCamera        = isFront,
        ) }

        refreshSupportedResolutions(isFront)  // ← sets selectedResolution from savedResolutionW/H
    }

    private fun savePreferences() {
        val context = getApplication<Application>().applicationContext
        val prefs = context.getSharedPreferences("stream_prefs", Context.MODE_PRIVATE)
        val state = _uiState.value

        prefs.edit().apply {
            putString("target_ip", state.targetIp)
            putString("resolution", "${state.selectedResolution.width}x${state.selectedResolution.height}")
            putInt("target_port", state.targetPort)
            putInt("bitrate_mbps", state.bitrateMbps)
            putBoolean("auto_discovery", state.autoDiscoveryEnabled)
            putBoolean("is_front_camera", state.isFrontCamera)
            apply()
        }
    }

    override fun onCleared() {
        handshakeServer.stop()
        networkDiscovery.stopAdvertising()
        super.onCleared()
    }
}

data class StreamUiState(
    val isStreaming: Boolean = false,
    val targetIp: String = "192.168.1.100",
    val targetPort: Int = 9000,
    val portText: String = "9000",
    val selectedResolution: CameraManager.ResolutionOption = CameraManager.ResolutionOption
        ("1080p", 1920, 1080, CameraManager.StreamResolution.RES_1080P),
    val supportedResolutions: List<CameraManager.ResolutionOption> = emptyList(),
    val frameCount: Long = 0,
    val fps: Int = 0,
    val bandwidth: Double = 0.0,
    val bitrateMbps: Int = 20,
    val statusMessage: String = "Ready to stream",
    val autoDiscoveryEnabled: Boolean = true,
    val isFrontCamera: Boolean = false,
    val isMicEnabled: Boolean = true,
    val password: String = ""
)