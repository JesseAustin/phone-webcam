package phone.webcam.phonewebcam.camera

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.*
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.view.Surface
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.concurrent.Executor
import kotlin.coroutines.resume

/**
 * CameraManager — fixed to support non-destructive preview surface attach/detach.
 *
 * Core design change:
 * ------------------
 * Previously, every call to startCapture() while already capturing would invoke
 * stopCapture() → cameraDevice.close(). This tore down the entire pipeline,
 * including the encoder (stream) surface, causing the H264 encoder to stop
 * receiving frames and the stream to die on every app foreground/background transition.
 *
 * The fix uses one of two strategies depending on API level:
 *
 *  - API 30+ : CaptureSession.updateOutputConfiguration() to hot-swap the preview
 *              surface in an OutputConfiguration that was pre-declared with a
 *              placeholder surface. Zero device close required.
 *
 *  - API 28-29: createCaptureSessionByOutputConfigurations() to rebuild the
 *               session without closing the CameraDevice. The encoder surface
 *               stays registered; only the session object is replaced.
 *
 *  - API < 28 : Falls back to createCaptureSession() without output configurations.
 *               Session is rebuilt (not device), so the encoder stays alive.
 *
 * The streamSurface (encoder input) is NEVER removed from the session while the
 * camera device is open. Only the previewSurface is added/removed dynamically.
 */
class CameraManager(private val context: Context) {

    enum class StreamResolution(val label: String, val width: Int, val height: Int) {
        RES_360P("360p", 640, 360),
        RES_480P("480p", 720, 480),
        RES_720P("720p", 1280, 720),
        RES_1080P("1080p", 1920, 1080),
        RES_1440P("2K", 2560, 1440),
        RES_2160P("4K", 3840, 2160),
        //RES_FULL_FOV("Native", 0, 0),
    }

    companion object {
        private const val TAG = "PhoneWebcam"
    }

    @Volatile private var captureGeneration = 0

    private val cameraManager =
        context.getSystemService(Context.CAMERA_SERVICE) as android.hardware.camera2.CameraManager

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null

    // Persistent references so we can reconfigure without re-opening the device
    private var activeStreamSurface: Surface? = null
    private var activePreviewSurface: Surface? = null

    // Held so updateOutputConfiguration() can find the right config by identity
    private var previewOutputConfig: OutputConfiguration? = null

    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    private var onDeviceClosed: (() -> Unit)? = null
    private var isCapturing = false

    // -------------------------------------------------------------------------
    // Camera discovery
    // -------------------------------------------------------------------------

    private fun findBackCamera(): String? =
        cameraManager.cameraIdList.find { id ->
            cameraManager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        }

    private fun findFrontCamera(): String? =
        cameraManager.cameraIdList.find { id ->
            cameraManager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        }

    /**
     * Returns a list of StreamResolutions that the current camera actually supports.
     */
    fun getSupportedResolutions(useFrontCamera: Boolean): List<ResolutionOption> {
        val cameraId = (if (useFrontCamera) findFrontCamera() else findBackCamera())
            ?: return emptyList()
        val chars = cameraManager.getCameraCharacteristics(cameraId)
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?: return emptyList()
        val supportedSizes = map.getOutputSizes(SurfaceTexture::class.java)
            ?: return emptyList()

        Log.d("CameraManager", "All supported sizes: ${supportedSizes.joinToString { "${it.width}x${it.height}" }}")

        val standard = StreamResolution.entries
            .filter { res ->
                res.width == 0 || supportedSizes.any { size ->
                    Math.abs(size.width - res.width).toFloat() / res.width < 0.15f &&
                            Math.abs(size.height - res.height).toFloat() / res.height < 0.15f
                }
            }
            .mapNotNull { res ->
                if (res.width == 0) {
                    ResolutionOption(res.label, 0, 0, res)
                } else {
                    val best = supportedSizes
                        .filter { size ->
                            Math.abs(size.width - res.width).toFloat() / res.width < 0.15f &&
                                    Math.abs(size.height - res.height).toFloat() / res.height < 0.15f
                        }
                        .minByOrNull { size ->
                            Math.abs(size.width - res.width) + Math.abs(size.height - res.height)
                        } ?: return@mapNotNull null
                    val exactMatch = best.width == res.width && best.height == res.height
                    val label = if (exactMatch) res.label else "${res.label} (${best.width} x ${best.height})"
                    ResolutionOption(label, best.width, best.height, res)
                }
            }
        return standard
    }

    data class ResolutionOption(
        val label: String,
        val width: Int,
        val height: Int,
        val enumValue: StreamResolution? = null  // null = Full FOV entry
    )

    /*
    fun setFrameRate(fps: Int) {
        val session = captureSession ?: return
        val device = cameraDevice ?: return
        val streamSurface = activeStreamSurface ?: return

        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(streamSurface)
                activePreviewSurface?.let { addTarget(it) }
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(fps, fps))
            }
            session.setRepeatingRequest(builder.build(), null, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "setFrameRate($fps) failed: ${e.message}")
        }
    }
    */

    // -------------------------------------------------------------------------
    // Background thread
    // -------------------------------------------------------------------------

    private fun startBackgroundThread() {
        if (backgroundThread != null) return
        backgroundThread = HandlerThread("CameraBackground").also { it.start() }
        backgroundHandler = Handler(backgroundThread!!.looper)
    }

    fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        try { backgroundThread?.join() } catch (e: InterruptedException) { }
        backgroundThread = null
        backgroundHandler = null
    }

    // -------------------------------------------------------------------------
    // Full stop (used only when stream is intentionally ended, not on UI hide)
    // -------------------------------------------------------------------------

    fun stopCapture(onClosed: (() -> Unit)? = null) {
        captureGeneration++
        isCapturing = false
        onDeviceClosed = onClosed
        activeStreamSurface = null
        activePreviewSurface = null
        previewOutputConfig = null

        val session = captureSession
        val device = cameraDevice
        captureSession = null
        cameraDevice = null

        try { session?.stopRepeating() } catch (e: Exception) { Log.w(TAG, "stopRepeating: ${e.message}") }
        try { session?.abortCaptures() } catch (e: Exception) { Log.w(TAG, "abortCaptures: ${e.message}") }
        try { session?.close() } catch (e: Exception) { Log.w(TAG, "session close: ${e.message}") }
        try { device?.close() } catch (e: Exception) { Log.w(TAG, "device close: ${e.message}") }

        if (device == null) {
            onDeviceClosed?.invoke()
            onDeviceClosed = null
        }
    }

    // -------------------------------------------------------------------------
    // Key public API: attach/detach preview WITHOUT touching CameraDevice
    // -------------------------------------------------------------------------

    /**
     * Call this when the UI becomes visible and provides a new SurfaceView surface.
     * The encoder surface is NOT touched. Only the session is updated.
     *
     * On API 30+ this is a zero-interruption hot-swap via updateOutputConfiguration().
     * On API 28-29 the session is rebuilt (no device close).
     * On API < 28 the session is rebuilt via the legacy path (no device close).
     */
    fun attachPreviewSurface(previewSurface: Surface) {
        val device = cameraDevice ?: run {
            Log.w(TAG, "attachPreviewSurface: no open device, ignoring")
            return
        }
        val streamSurface = activeStreamSurface ?: run {
            Log.w(TAG, "attachPreviewSurface: no active stream surface, ignoring")
            return
        }
        if (!previewSurface.isValid) {
            Log.w(TAG, "attachPreviewSurface: surface is not valid yet")
            return
        }

        Log.i(TAG, "attachPreviewSurface: reconfiguring session (API ${Build.VERSION.SDK_INT})")
        activePreviewSurface = previewSurface

        when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> {
                // API 30+: hot-swap via updateOutputConfiguration — zero stream interruption
                hotSwapPreviewSurface(previewSurface)
            }
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> {
                // API 28-29: rebuild session with OutputConfigurations, device stays open
                rebuildSessionWithOutputConfigurations(device, streamSurface, previewSurface)
            }
            else -> {
                // API < 28: rebuild session via legacy API, device stays open
                rebuildSessionLegacy(device, listOf(streamSurface, previewSurface))
            }
        }
    }

    /**
     * Call this when the UI is hidden (app minimised / surface destroyed).
     * Removes the preview surface from the session so the SurfaceView can be
     * safely destroyed without producing "BufferQueue has been abandoned" errors.
     * The encoder surface and CameraDevice are untouched.
     */
    fun detachPreviewSurface() {
        val session = captureSession ?: run {
            Log.w(TAG, "detachPreviewSurface: no session, bailing")
            return
        }
        val device = cameraDevice ?: run {
            Log.w(TAG, "detachPreviewSurface: no device, bailing")
            return
        }
        val streamSurface = activeStreamSurface ?: run {
            Log.w(TAG, "detachPreviewSurface: no stream surface, bailing")
            return
        }
        val previewSurface = activePreviewSurface ?: run {
            Log.d(TAG, "detachPreviewSurface: no preview surface, nothing to do")
            return
        }

        Log.i(TAG, "detachPreviewSurface: dropping preview from repeating request ONLY — no session rebuild")
        activePreviewSurface = null
        previewOutputConfig = null

        try {
            session.stopRepeating()
            startRepeatingRequest(session, device, listOf(streamSurface))
            // Release the surface so the HAL stops queuing frames into the dead BufferQueue
            previewSurface.release()
            Log.i(TAG, "detachPreviewSurface: success")
        } catch (e: Exception) {
            Log.e(TAG, "detachPreviewSurface: failed — ${e.message}")
        }
    }

    // -------------------------------------------------------------------------
    // Initial startCapture (opens CameraDevice for the first time / after full stop)
    // -------------------------------------------------------------------------

    suspend fun startCapture(
        width: Int = 1920,
        height: Int = 1080,
        streamSurface: Surface? = null,
        previewSurface: Surface? = null,
        useFrontCamera: Boolean = false,
    ) = suspendCancellableCoroutine<Unit> { continuation ->

        val myGeneration = ++captureGeneration
        startBackgroundThread()

        val cameraId = (if (useFrontCamera) findFrontCamera() else findBackCamera()) ?: run {
            continuation.cancel(); return@suspendCancellableCoroutine
        }

        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            continuation.cancel(SecurityException("Camera permission not granted"))
            return@suspendCancellableCoroutine
        }

        fun openCamera() {
            try {
                cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        if (myGeneration != captureGeneration) {
                            camera.close(); continuation.cancel(); return
                        }
                        cameraDevice = camera
                        activeStreamSurface = streamSurface
                        activePreviewSurface = previewSurface

                        val targets = mutableListOf<Surface>()
                        streamSurface?.takeIf { it.isValid }?.let { targets.add(it) }
                            ?: Log.w(TAG, "Stream surface invalid or null")
                        previewSurface?.takeIf { it.isValid }?.let { targets.add(it) }

                        if (targets.isEmpty()) {
                            Log.e(TAG, "No valid surfaces to start capture")
                            continuation.cancel(); return
                        }

                        Log.i(TAG, "openCamera: ${targets.size} surfaces (stream=${streamSurface?.isValid}, preview=${previewSurface?.isValid})")

                        val onResult: (Boolean) -> Unit = { success ->
                            if (success) {
                                isCapturing = true
                                Log.i(TAG, "Capture session started successfully")
                                if (continuation.isActive) continuation.resume(Unit)
                            } else {
                                Log.e(TAG, "Failed to configure capture session")
                                continuation.cancel()
                            }
                        }

                        when {
                            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ->
                                createSessionWithOutputConfigurations(camera, targets, onResult)
                            else ->
                                createSessionLegacy(camera, targets, onResult)
                        }
                    }

                    override fun onClosed(camera: CameraDevice) {
                        onDeviceClosed?.invoke(); onDeviceClosed = null
                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        camera.close(); cameraDevice = null; continuation.cancel()
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        camera.close(); cameraDevice = null
                        continuation.cancel(RuntimeException("Camera error $error"))
                    }
                }, backgroundHandler)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to open camera", e)
                continuation.cancel(e)
            }
        }

        // CHANGED: if already capturing, do a full stop only when a full restart is genuinely
        // needed (e.g. camera flip). For preview-surface-only changes, use attach/detachPreviewSurface.
        if (isCapturing) {
            Log.w(TAG, "startCapture called while capturing — doing full restart")
            stopCapture(onClosed = { openCamera() })
        } else {
            openCamera()
        }

        continuation.invokeOnCancellation { stopCapture() }
    }

    // -------------------------------------------------------------------------
    // API 30+ hot-swap: updateOutputConfiguration
    // -------------------------------------------------------------------------

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.R)
    private fun hotSwapPreviewSurface(newSurface: Surface) {
        val session = captureSession ?: run {
            Log.w(TAG, "hotSwapPreviewSurface: no active session"); return
        }
        val config = previewOutputConfig

        try {
            if (config != null) {
                // OutputConfiguration.surface is val — create a new config with the new surface
                // and update the session. The new config must match the same physical camera
                // and size as the original.
                val newConfig = OutputConfiguration(newSurface)
                previewOutputConfig = newConfig
                session.updateOutputConfiguration(newConfig)
                Log.i(TAG, "hotSwapPreviewSurface: updateOutputConfiguration succeeded")
            } else {
                // Preview config doesn't exist yet — rebuild session (first attach after open)
                val device = cameraDevice ?: return
                val streamSurface = activeStreamSurface ?: return
                rebuildSessionWithOutputConfigurations(device, streamSurface, newSurface)
                return
            }
            // Re-issue the repeating request so the new surface starts receiving frames
            restartRepeatingRequest()
        } catch (e: Exception) {
            Log.e(TAG, "hotSwapPreviewSurface failed, falling back to session rebuild", e)
            val device = cameraDevice ?: return
            val streamSurface = activeStreamSurface ?: return
            rebuildSessionWithOutputConfigurations(device, streamSurface, newSurface)
        }
    }

    // -------------------------------------------------------------------------
    // API 28+ session rebuild (OutputConfigurations) — no device close
    // -------------------------------------------------------------------------

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
    private fun createSessionWithOutputConfigurations(
        camera: CameraDevice,
        surfaces: List<Surface>,
        callback: (Boolean) -> Unit
    ) {
        try {
            val outputConfigs = surfaces.map { surface ->
                OutputConfiguration(surface).also { config ->
                    // Track the preview config for later hot-swap / detach
                    if (surface != activeStreamSurface) previewOutputConfig = config
                }
            }
            buildSessionFromConfigs(camera, outputConfigs, callback)
        } catch (e: Exception) {
            Log.e(TAG, "createSessionWithOutputConfigurations failed", e)
            callback(false)
        }
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
    private fun rebuildSessionWithOutputConfigurations(
        camera: CameraDevice,
        streamSurface: Surface,
        previewSurface: Surface?
    ) {
        try {
            val sessionRef = captureSession
            try { sessionRef?.stopRepeating() } catch (e: Exception) { /* ignore */ }
            // Do NOT close the session here if on API 28/29 — closing it while rebuilding
            // can cause the device to think all outputs are gone. Let the new session replace it.

            val configs = mutableListOf(OutputConfiguration(streamSurface))
            if (previewSurface != null && previewSurface.isValid) {
                val previewConfig = OutputConfiguration(previewSurface)
                previewOutputConfig = previewConfig
                configs.add(previewConfig)
            } else {
                previewOutputConfig = null
            }

            buildSessionFromConfigs(camera, configs) { success: Boolean ->
                if (success) {
                    // Safe to close the old session now that the new one is configured
                    try { sessionRef?.close() } catch (e: Exception) { /* ignore */ }
                    Log.i(TAG, "rebuildSession (OutputConfigurations): success, preview=${previewSurface != null}")
                } else {
                    Log.e(TAG, "rebuildSession (OutputConfigurations): failed")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "rebuildSessionWithOutputConfigurations error", e)
        }
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
    private fun buildSessionFromConfigs(
        camera: CameraDevice,
        outputConfigs: List<OutputConfiguration>,
        callback: (Boolean) -> Unit
    ) {
        val executor: Executor = Executor { command -> backgroundHandler?.post(command) ?: command.run() }
        val sessionConfig = SessionConfiguration(
            SessionConfiguration.SESSION_REGULAR,
            outputConfigs,
            executor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        val srfs = outputConfigs.mapNotNull { it.surface }.filter { it.isValid }
                        startRepeatingRequest(session, camera, srfs)
                        callback(true)
                    } catch (e: Exception) {
                        Log.e(TAG, "Error starting repeating request", e)
                        callback(false)
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "buildSessionFromConfigs: onConfigureFailed")
                    callback(false)
                }
            }
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            val chars = cameraManager.getCameraCharacteristics(camera.id)
            val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            val supports4K = caps?.contains(
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO
            ) == true
            val maxSize = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(ImageFormat.JPEG)
                ?.maxByOrNull { it.width * it.height }
            Log.d(TAG, "Camera max size: $maxSize, supportsHighSpeed: $supports4K")
        }

        camera.createCaptureSession(sessionConfig)
    }

    // -------------------------------------------------------------------------
    // Legacy session (API < 28) — no device close, just session replace
    // -------------------------------------------------------------------------

    private fun createSessionLegacy(
        camera: CameraDevice,
        surfaces: List<Surface>,
        callback: (Boolean) -> Unit
    ) {
        try {
            val valid = surfaces.filter { it.isValid }
            if (valid.isEmpty()) { callback(false); return }
            camera.createCaptureSession(valid, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        startRepeatingRequest(session, camera, valid)
                        callback(true)
                    } catch (e: Exception) {
                        Log.e(TAG, "Legacy session repeating request failed", e)
                        callback(false)
                    }
                }
                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "createSessionLegacy: onConfigureFailed")
                    callback(false)
                }
            }, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "createSessionLegacy error", e)
            callback(false)
        }
    }

    private fun rebuildSessionLegacy(camera: CameraDevice, surfaces: List<Surface>) {
        val oldSession = captureSession
        try { oldSession?.stopRepeating() } catch (e: Exception) { /* ignore */ }

        val valid = surfaces.filter { it.isValid }
        if (valid.isEmpty()) { Log.w(TAG, "rebuildSessionLegacy: no valid surfaces"); return }

        createSessionLegacy(camera, valid) { success: Boolean ->
            if (success) {
                try { oldSession?.close() } catch (e: Exception) { /* ignore */ }
                Log.i(TAG, "rebuildSessionLegacy: success, surfaces=${valid.size}")
            } else {
                Log.e(TAG, "rebuildSessionLegacy: failed")
            }
        }
    }

    // -------------------------------------------------------------------------
    // Shared repeating request helpers
    // -------------------------------------------------------------------------

    private fun startRepeatingRequest(
        session: CameraCaptureSession,
        camera: CameraDevice,
        surfaces: List<Surface>
    ) {
        val builder = try {
            camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
        } catch (e: CameraAccessException) {
            Log.w(TAG, "TEMPLATE_RECORD unsupported, falling back to TEMPLATE_PREVIEW")
            camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
        }

        // Only add surfaces that are registered in this session
        surfaces.filter { it.isValid }.forEach { builder.addTarget(it) }

        builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(24, 30))

        session.setRepeatingRequest(
            builder.build(),
            object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureFailed(
                    session: CameraCaptureSession,
                    request: CaptureRequest,
                    failure: CaptureFailure
                ) {
                    // Suppress — preview surface may disappear when app is backgrounded
                }
            },
            backgroundHandler
        )
    }

    /** Re-issues the repeating request on the existing session (used after hot-swap). */
    private fun restartRepeatingRequest() {
        val session = captureSession ?: return
        val device = cameraDevice ?: return
        try {
            session.stopRepeating()
            val surfaces = listOfNotNull(activeStreamSurface, activePreviewSurface)
            startRepeatingRequest(session, device, surfaces)
            Log.i(TAG, "restartRepeatingRequest: done")
        } catch (e: Exception) {
            Log.e(TAG, "restartRepeatingRequest failed", e)
        }
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /** True once CameraDevice is open and a capture session is active. */
    fun isDeviceOpen(): Boolean = cameraDevice != null && captureSession != null

    fun release() {
        stopCapture()
        stopBackgroundThread()
    }
}
