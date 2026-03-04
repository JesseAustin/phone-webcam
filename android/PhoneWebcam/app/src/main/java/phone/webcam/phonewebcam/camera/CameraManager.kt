package phone.webcam.phonewebcam.camera

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.*
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

class CameraManager(private val context: Context) {

    enum class StreamResolution(val label: String, val width: Int, val height: Int) {
        RES_480P("480p", 720, 480),
        RES_720P("720p", 1280, 720),
        RES_1080P("1080p", 1920, 1080),
        RES_2160P("2K", 3840, 2160),
        //RES_4320P("4320p", 8640, 4320)
    }

    companion object {
        private const val TAG = "PhoneWebcam"
        private const val FPS_MIN = 15
        private const val FPS_MAX = 30
    }

    //private val useFrontCamera = false
    @Volatile private var captureGeneration = 0
    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as android.hardware.camera2.CameraManager
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    private var onDeviceClosed: (() -> Unit)? = null
    private var isCapturing = false

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

    fun stopCapture(onClosed: (() -> Unit)? = null) {
        captureGeneration++  // invalidate any in-flight startCapture
        isCapturing = false
        onDeviceClosed = onClosed

        val session = captureSession
        val device = cameraDevice
        captureSession = null
        cameraDevice = null

        try { session?.stopRepeating() } catch (e: Exception) { Log.w(TAG, "stopRepeating: ${e.message}") }
        try { session?.abortCaptures() } catch (e: Exception) { Log.w(TAG, "abortCaptures: ${e.message}") }
        try { session?.close() } catch (e: Exception) { Log.w(TAG, "session close: ${e.message}") }
        try { device?.close() } catch (e: Exception) { Log.w(TAG, "device close: ${e.message}") }

        // If there was no device, onClosed will never fire from the callback — invoke immediately
        if (device == null) {
            onDeviceClosed?.invoke()
            onDeviceClosed = null
        }
    }

    suspend fun startCapture(
        resolution: StreamResolution = StreamResolution.RES_1080P,
        streamSurface: Surface? = null,
        previewSurface: Surface? = null,
        useFrontCamera: Boolean = false,
    ) = suspendCancellableCoroutine<Unit> { continuation ->

        val myGeneration = ++captureGeneration

        startBackgroundThread()

        // Check the boolean flag to see if the user has selected the front or back facing camera:
        val cameraId = (if (useFrontCamera) findFrontCamera() else findBackCamera()) ?: run {
            continuation.cancel()
            return@suspendCancellableCoroutine
        }


        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            continuation.cancel(SecurityException("Camera permission not granted"))
            return@suspendCancellableCoroutine
        }

        fun openCamera() {
            try {
                cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {

                        // Abort if a newer capture was requested since we started
                        if (myGeneration != captureGeneration) {
                            camera.close()
                            continuation.cancel()
                            return
                        }

                        cameraDevice = camera

                        val targets = mutableListOf<Surface>()
                        streamSurface?.let {
                            if (it.isValid) targets.add(it)
                            else Log.w(TAG, "Stream surface provided but is NOT valid")
                        }
                        previewSurface?.let {
                            if (it.isValid) targets.add(it)
                            else Log.w(TAG, "Preview surface provided but is NOT valid")
                        }

                        if (targets.isEmpty()) {
                            Log.e(TAG, "No valid surfaces available to start capture.")
                            continuation.cancel()
                            return
                        }

                        createCaptureSession(camera, targets) { success ->
                            if (success) {
                                isCapturing = true
                                Log.i(TAG, "Camera capture session started successfully")
                                if (continuation.isActive) continuation.resume(Unit)
                            } else {
                                Log.e(TAG, "Failed to configure capture session")
                                continuation.cancel()
                            }
                        }
                    }

                    override fun onClosed(camera: CameraDevice) {
                        onDeviceClosed?.invoke()
                        onDeviceClosed = null
                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        camera.close()
                        cameraDevice = null
                        continuation.cancel()
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        camera.close()
                        cameraDevice = null
                        continuation.cancel(RuntimeException("Camera error $error"))
                    }
                }, backgroundHandler)
            } catch (e: CameraAccessException) {
                Log.e(TAG, "Failed to open camera: ${e.message}")
                continuation.cancel(e)
            } catch (e: Exception) {
                continuation.cancel(e)
            }
        }

        if (isCapturing) {
            Log.w(TAG, "startCapture called while already capturing — waiting for device close")
            stopCapture(onClosed = { openCamera() })
        } else {
            openCamera()
        }

        continuation.invokeOnCancellation { stopCapture() }
    }

    private fun createCaptureSession(
        camera: CameraDevice,
        surfaces: List<Surface>,
        callback: (Boolean) -> Unit
    ) {
        try {
            val validSurfaces = surfaces.filter { it.isValid }
            if (validSurfaces.isEmpty()) {
                Log.e(TAG, "No valid surfaces! Aborting session.")
                callback(false)
                return
            }

            camera.createCaptureSession(validSurfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        val builder = try {
                            camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
                        } catch (e: CameraAccessException) {
                            Log.w(TAG, "TEMPLATE_RECORD not supported, falling back to TEMPLATE_PREVIEW")
                            camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
                        }
                        validSurfaces.forEach { builder.addTarget(it) }

                        builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                        builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(24, 30))

                        session.setRepeatingRequest(builder.build(), null, backgroundHandler)
                        callback(true)
                    } catch (e: Exception) {
                        Log.e(TAG, "Error starting repeating request", e)
                        callback(false)
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Failed to configure capture session")
                    callback(false)
                }
            }, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Error creating capture session", e)
            callback(false)
        }
    }

    fun release() {
        stopCapture()
        stopBackgroundThread()
    }

    private fun executeHardwareShutdown() {
        try {
            captureSession?.stopRepeating()
            captureSession?.abortCaptures()
            captureSession?.close()
            cameraDevice?.close()
        } catch (e: Exception) {
            Log.e(TAG, "Shutdown error", e)
        } finally {
            captureSession = null
            cameraDevice = null
        }
    }
}