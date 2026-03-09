package phone.webcam.phonewebcam.service

import android.app.*
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaRecorder
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.*
import phone.webcam.phonewebcam.R
import phone.webcam.phonewebcam.camera.CameraManager
import phone.webcam.phonewebcam.network.RtpSender
import phone.webcam.phonewebcam.encoder.H264Encoder
import phone.webcam.phonewebcam.security.SrtpKeyDerivation
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicLong


class CameraStreamService : Service() {

    companion object {
        private const val TAG = "PhoneWebcam"

        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID      = "phone_webcam_stream"
        private const val CHANNEL_NAME    = "Camera Stream"

        const val ACTION_START_STREAM = "phone.webcam.START_STREAM"
        const val ACTION_STOP_STREAM  = "phone.webcam.STOP_STREAM"
        const val EXTRA_TARGET_IP     = "target_ip"
        const val EXTRA_TARGET_PORT   = "target_port"
        const val EXTRA_RESOLUTION    = "resolution"
        const val EXTRA_BITRATE_MBPS  = "extra_bitrate_mbps"
        const val EXTRA_PASSWORD      = "extra_password"
        const val EXTRA_IS_FRONT_CAMERA = "extra_is_front_camera"
        const val EXTRA_SRTP_SALT = "extra_srtp_salt"
    }

    private var lastFrameSentMs = 0L
    private val binder = LocalBinder()
    private var cameraManager: CameraManager? = null

    private var rtpSender: RtpSender? = null
    private var audioSender: RtpSender? = null
    private var h264Encoder: H264Encoder? = null
    private var drainJob: Job? = null
    private var drainThread: Thread? = null
    private var serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @Volatile private var isStreaming = false
    @Volatile private var previewSurface: Surface? = null
    private val senderLock = Any()

    private val frameCount = AtomicLong(0)
    private val bytesSent  = AtomicLong(0)
    private var startTime  = 0L
    private var selectedWidth: Int = 1920
    private var selectedHeight: Int = 1080
    private var bitrateMbps: Int = 20
    private var currentPassword: String = ""
    private var currentSrtpSalt: ByteArray? = null

    @Volatile private var audioRecord: AudioRecord? = null
    @Volatile private var audioCodec: MediaCodec? = null
    @Volatile private var audioThread: Thread? = null
    private var isMicEnabled = true

    fun setMicEnabled(enabled: Boolean) {
        isMicEnabled = enabled
    }

    fun startAudioStream(host: String, port: Int) {
        val sampleRate = 44100
        val channelConfig = AudioFormat.CHANNEL_IN_MONO
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)

        // Check permission FIRST
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "RECORD_AUDIO permission not granted")
            return
        }

        // Create AudioRecord
        audioRecord = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            sampleRate,
            channelConfig,
            audioFormat,
            minBufSize * 4
        )

        // Verify initialization
        if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord failed to initialize")
            audioRecord?.release()
            audioRecord = null
            return
        }

        // Set up the RtpSender for audio
        synchronized(senderLock) {
            audioSender?.close()
            audioSender = createRtpSender(host, port)
        }

        // Now create codec
        audioCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).also { codec ->
            val format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AAC,
                sampleRate,
                1
            )
            format.setInteger(MediaFormat.KEY_BIT_RATE, 64000)
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, minBufSize * 4)
            format.setInteger(
                MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC
            )
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
        }

        // SAFE to start recording
        audioRecord?.startRecording()

        audioThread = Thread {
            val FRAME_SAMPLES = 1024
            val pcmBuf = ByteArray(FRAME_SAMPLES * 2)  // 16-bit = 2 bytes per sample
            val bufInfo = MediaCodec.BufferInfo()

            while (!Thread.currentThread().isInterrupted) {
                try {
                    val codec = audioCodec ?: break
                    val record = audioRecord ?: break

                    // Feed PCM into encoder
                    val inputIdx = codec.dequeueInputBuffer(10000)
                    if (inputIdx >= 0) {
                        val inputBuf = codec.getInputBuffer(inputIdx) ?: continue
                        val bytesRead = if (isMicEnabled) {
                            record.read(pcmBuf, 0, pcmBuf.size)
                        } else {
                            // Send silence when muted
                            pcmBuf.fill(0)
                            pcmBuf.size
                        }

                        if (bytesRead > 0) {
                            inputBuf.clear()
                            inputBuf.put(pcmBuf, 0, bytesRead)
                            codec.queueInputBuffer(inputIdx, 0, bytesRead, System.nanoTime() / 1000, 0)
                        }
                    }

                    // Get encoded output and send via RTP
                    val outputIdx = codec.dequeueOutputBuffer(bufInfo, 10000)
                    if (outputIdx >= 0) {
                        val outputBuf = codec.getOutputBuffer(outputIdx) ?: continue
                        val aacData = ByteArray(bufInfo.size)
                        outputBuf.get(aacData)
                        codec.releaseOutputBuffer(outputIdx, false)

                        // Skip codec config packets
                        if (bufInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) continue

                        // Simple RTP packet: ADTS header + AAC data
                        val adts = makeAdtsHeader(aacData.size, sampleRate, 1)
                        val payload = adts + aacData
                        val rtpTimestamp = (bufInfo.presentationTimeUs * 44100 / 1_000_000).toInt()

                        synchronized(senderLock) {
                            audioSender?.sendRtp(payload, rtpTimestamp, 97)
                        }
                    }
                } catch (e: IllegalStateException) {
                    // Buffer becomes inaccessible if codec is stopped
                    break
                } catch (e: Exception) {
                    Log.e(TAG, "Audio thread loop error: ${e.message}")
                    break
                }
            }
        }.also { it.start() }
    }

    /*
    fun dropTo15Fps() {
        // Android will throttle camera to ~15fps in background anyway.
        // Explicitly request 15fps so the encoder doesn't build a backlog.
        cameraManager?.setFrameRate(15)
    }

    fun returnTo30Fps() {
        cameraManager?.setFrameRate(30)
        requestKeyFrame()
    }
    */

    fun stopAudioStream() {
        audioThread?.interrupt()
        audioThread?.join()
        audioThread = null

        val record = audioRecord
        audioRecord = null
        try {
            if (record?.state == AudioRecord.STATE_INITIALIZED) {
                record.stop()
            }
            record?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioRecord: ${e.message}")
        }

        val codec = audioCodec
        audioCodec = null
        try {
            codec?.stop()
            codec?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioCodec: ${e.message}")
        }

        synchronized(senderLock) {
            audioSender?.close()
            audioSender = null
        }
    }

    private fun makeAdtsHeader(aacLength: Int, sampleRate: Int, channels: Int): ByteArray {
        val freqIdx = when (sampleRate) {
            96000 -> 0; 88200 -> 1; 64000 -> 2; 48000 -> 3
            44100 -> 4; 32000 -> 5; 24000 -> 6; 22050 -> 7
            16000 -> 8; 12000 -> 9; 11025 -> 10; 8000 -> 11
            else -> 4
        }
        val frameLen = aacLength + 7
        return byteArrayOf(
            0xFF.toByte(), 0xF1.toByte(),
            (0x40 or (freqIdx shl 2) or (channels shr 2)).toByte(),
            ((channels and 3 shl 6) or (frameLen shr 11)).toByte(),
            (frameLen shr 3 and 0xFF).toByte(),
            (frameLen and 7 shl 5 or 0x1F).toByte(),
            0xFC.toByte()
        )
    }

    inner class LocalBinder : Binder() {
        fun getService(): CameraStreamService = this@CameraStreamService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()

        // Ensure scope is alive if service is restarted
        if (!serviceScope.isActive) {
            serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        }

        createNotificationChannel()
        cameraManager = CameraManager(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_STREAM -> {
                Log.i(TAG, "ACTION_START_STREAM received — isStreaming=$isStreaming")

                val ip   = intent.getStringExtra(EXTRA_TARGET_IP) ?: "192.168.1.100"
                val port = intent.getIntExtra(EXTRA_TARGET_PORT, 9000)
                val resString = intent.getStringExtra(EXTRA_RESOLUTION) ?: "1920x1080"

                bitrateMbps = intent.getIntExtra(EXTRA_BITRATE_MBPS, 20)
                currentPassword = intent.getStringExtra(EXTRA_PASSWORD) ?: ""

                // Decode the hex salt back to bytes
                val saltHex = intent.getStringExtra(EXTRA_SRTP_SALT) ?: ""
                currentSrtpSalt = if (saltHex.isNotEmpty()) {
                    saltHex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
                } else null

                val parts = resString.split("x")
                val resW = parts.getOrNull(0)?.toIntOrNull() ?: 1920
                val resH = parts.getOrNull(1)?.toIntOrNull() ?: 1080

                val isFrontCamera = intent.getBooleanExtra(EXTRA_IS_FRONT_CAMERA, false)
                startStreaming(ip, port, resW, resH, isFrontCamera)
            }
            ACTION_STOP_STREAM -> {
                Log.i(TAG, "Stop requested via Intent (Notification)")
                stopStreaming()
            }
        }
        return START_NOT_STICKY
    }

    fun setPreviewSurface(surface: Surface?) {
        previewSurface = surface

        if (!isStreaming) {
            // Camera not open yet — just store it. startCapture() will pass it when it runs.
            Log.i(TAG, "setPreviewSurface: not streaming yet, stored for startCapture")
            return
        }

        if (surface != null) {
            val hasOpenDevice = cameraManager?.isDeviceOpen() == true
            if (hasOpenDevice) {
                // Camera already open — hot-attach the preview surface
                Log.i(TAG, "setPreviewSurface: camera open, attaching preview")
                cameraManager?.attachPreviewSurface(surface)
            } else {
                // isStreaming=true but camera not open yet (still in startCapture coroutine).
                // previewSurface is already stored above; startCapture will pick it up.
                Log.i(TAG, "setPreviewSurface: streaming but camera not open yet, stored for startCapture")
            }
        } else {
            // UI going away — remove preview surface so the SurfaceView can be safely destroyed
            cameraManager?.detachPreviewSurface()
        }
    }

    fun switchCamera(useFront: Boolean, onComplete: ((Boolean) -> Unit)? = null) {
        if (!isStreaming) { onComplete?.invoke(false); return }

        val encoderSurface = h264Encoder?.inputSurface
        if (encoderSurface == null) { onComplete?.invoke(false); return }

        serviceScope.launch {
            try {
                cameraManager?.stopCapture()
                delay(300) // let camera session fully close
                cameraManager?.startCapture(
                    width = selectedWidth,
                    height = selectedHeight,
                    streamSurface = encoderSurface,
                    previewSurface = previewSurface,
                    useFrontCamera = useFront
                )
                delay(100) // let first frames through before keyframe
                requestKeyFrame()
            } catch (e: Exception) {
                Log.e(TAG, "Camera switch failed: ${e.message}")
                // Try to recover by restarting with back camera
                try {
                    cameraManager?.startCapture(
                        width = selectedWidth,
                        height = selectedHeight,
                        streamSurface = h264Encoder?.inputSurface,
                        previewSurface = previewSurface,
                        useFrontCamera = false
                    )
                } catch (e2: Exception) {
                    Log.e(TAG, "Camera recovery failed: ${e2.message}")
                }
            }
        }
    }

    fun requestKeyFrame() {
        h264Encoder?.requestKeyFrame()
    }

    fun setRtpDestination(ip: String, port: Int) {
        synchronized(senderLock) {
            rtpSender?.close()
            rtpSender = createRtpSender(ip, port)
            audioSender?.close()
            audioSender = createRtpSender(ip, port + 1)
        }
        requestKeyFrame()
    }

    fun redirectStream(newIp: String, newPort: Int) {
        if (!isStreaming) return

        serviceScope.launch {
            // give OBS time to finish SRTP session init
            delay(370)
            val oldSender: RtpSender?
            val oldAudioSender: RtpSender?
            synchronized(senderLock) {
                oldSender = rtpSender
                rtpSender = createRtpSender(newIp, newPort)
                oldAudioSender = audioSender
                audioSender = createRtpSender(newIp, newPort + 1)
            }
            oldSender?.close()
            oldAudioSender?.close()

            val encoder = h264Encoder
            val sender  = synchronized(senderLock) { rtpSender }
            if (encoder != null && sender != null) {
                encoder.cachedSps?.let { sps ->
                    extractNalsFromAnnexB(sps).forEach { sender.sendNal(it, 0, true) }
                }
                encoder.cachedPps?.let { pps ->
                    extractNalsFromAnnexB(pps).forEach { sender.sendNal(it, 0, true) }
                }
            }
            requestKeyFrame()
            updateNotification("Streaming to $newIp:$newPort")
        }
    }

    private fun createRtpSender(ip: String, port: Int): RtpSender {
        Log.d("RtpSender", "createRtpSender: password empty=${currentPassword.isEmpty()}")
        Log.d(TAG, "createRtpSender called from: ${Thread.currentThread().stackTrace.take(6).joinToString(" <- ") { it.methodName }}")

        rtpSender?.reset()

        return if (currentPassword.isNotEmpty()) {
            val salt = currentSrtpSalt ?: java.security.MessageDigest.getInstance("SHA-256")
                .digest(currentPassword.toByteArray())
                .take(16)
                .toByteArray()  // fallback if no salt received
            val (key, rtpSalt) = SrtpKeyDerivation.deriveKeys(currentPassword, salt)
            RtpSender(ip, port, key, rtpSalt)
        } else {
            RtpSender(ip, port)
        }
    }

    fun startStreaming(
        targetIp: String,
        targetPort: Int,
        resolutionWidth: Int,
        resolutionHeight: Int,
        useFrontCamera: Boolean = false,
    ) {
        frameCount.set(0)
        bytesSent.set(0)
        startTime = System.currentTimeMillis()

        serviceScope.launch {
            try {
                selectedWidth = resolutionWidth
                selectedHeight = resolutionHeight

                // Set thread priority:
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_VIDEO)

                if (isStreaming) {
                    teardown()
                }

                isStreaming = false

                val notif = createNotification("Connecting...")
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                    startForeground(NOTIFICATION_ID, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
                } else {
                    startForeground(NOTIFICATION_ID, notif)
                }

                synchronized(senderLock) {
                    rtpSender?.close()
                    rtpSender = createRtpSender(targetIp, targetPort)
                }

                h264Encoder = H264Encoder(
                    selectedWidth,
                    selectedHeight,
                    bitrateMbps * 1_000_000
                ).also { encoder ->

                    var lastPts = 0L
                    var firstFrameWallNs = 0L
                    var firstFramePts = 0L

                    encoder.onEncodedFrame = { nal, pts, isLast ->

                        bytesSent.addAndGet(nal.size.toLong())
                        if (isLast) frameCount.incrementAndGet()

                        if (isLast) {
                            val ptsDelta = pts - lastPts
                            if (lastPts != 0L && (ptsDelta !in 20_000..60_000)) {
                                Log.w(TAG, "PTS_ANOMALY: delta=${ptsDelta}us (expected ~33333us)")
                            }
                            lastPts = pts

                            val now = System.currentTimeMillis()
                            val interval = now - lastFrameSentMs
                            lastFrameSentMs = now
                            if (interval > 50) {
                                Log.w(TAG, "FRAME_GAP: ${interval}ms between frames")
                            }
                        }

                        val wallNow = System.nanoTime()
                        if (firstFrameWallNs == 0L) {
                            firstFrameWallNs = wallNow
                            firstFramePts = pts
                        } else {
                            val ptsDrift = ((wallNow - firstFrameWallNs) / 1000L) - (pts - firstFramePts)
                            if (pts < firstFramePts || ptsDrift > 1_500_000L || ptsDrift < -500_000L) {
                                Log.w(TAG, "PTS_REANCHOR: drift=${ptsDrift}us")
                                firstFrameWallNs = wallNow
                                firstFramePts = pts
                            }
                        }

                        val wallPts = firstFramePts + (wallNow - firstFrameWallNs) / 1000L
                        val wallNowUs = wallNow / 1000L

                        // Drop frames more than 200ms behind wall clock
                        val wallElapsedUs = (wallNow - firstFrameWallNs) / 1000L
                        val ptsElapsedUs = pts - firstFramePts
                        val staleness = wallElapsedUs - ptsElapsedUs

                        if (staleness <= 200_000L) {
                            val rtpTimestamp = (wallPts * 90L / 1000L).toInt()
                            synchronized(senderLock) {
                                rtpSender?.sendNal(nal, rtpTimestamp, isLast)
                            }
                        }

                    }
                    encoder.start()
                    encoder.requestKeyFrame()
                }

                isStreaming = true

                // STOP using serviceScope.launch(Dispatchers.Default) for the drain loop
                drainThread = Thread({
                    android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_VIDEO)
                    try {
                        while (isStreaming && !Thread.currentThread().isInterrupted) {
                            val encoder = h264Encoder
                            if (encoder != null) {
                                encoder.drainOutput()
                                // A very short sleep provides backpressure without high CPU usage
                                Thread.sleep(1)
                            } else {
                                Thread.sleep(10)
                            }
                        }
                    } catch (e: InterruptedException) {
                        // Normal exit on stop
                    } catch (e: Exception) {
                        Log.e(TAG, "Drain thread error", e)
                    }
                }, "EncoderDrainThread").apply {
                    isDaemon = true
                    start()
                }

                updateNotification("Streaming to $targetIp:$targetPort")

                cameraManager?.startCapture(
                    width = selectedWidth,
                    height = selectedHeight,
                    h264Encoder?.inputSurface,
                    previewSurface,
                    useFrontCamera = useFrontCamera
                )

                startAudioStream(targetIp, targetPort + 1)

            } catch (e: Exception) {
                Log.e(TAG, "Pipeline error", e)
                //isStreaming = true  // force teardown to actually run
                stopStreaming()
            }
        }
    }

    @Volatile private var isTearingDown = false

    private suspend fun teardown() {
        // 1. Interrupt an stop the drain loop immediately
        drainThread?.let { thread ->
            thread.interrupt()
            // withContext returns 'R'. By naming the parameter 'thread',
            // we help the compiler realize this block returns Unit.
            withContext(Dispatchers.IO) {
                try {
                    thread.join(500)
                } catch (e: Exception) {
                    Log.e(TAG, "Join interrupted: ${e.message}")
                }
            }
        }
        drainThread = null

        // Collapse concurrent teardown calls — only one runs at a time
        if (isTearingDown) {
            Log.w(TAG, "teardown: already in progress, skipping")
            return
        }
        isTearingDown = true

        try {
            // Already done earlier in "stopStreaming()"
            // so we no longer need it here:
            //isStreaming = false

            // Stop audio first (fast — just interrupts a thread)
            withContext(Dispatchers.IO) {
                stopAudioStream()
            }

            // Stop camera (async close — must complete before encoder stop)
            withContext(Dispatchers.IO) {
                cameraManager?.stopCapture()
            }

            // Stop encoder only after camera is closed so the input surface
            // is no longer being written to
            h264Encoder?.stop()
            h264Encoder = null

            synchronized(senderLock) {
                rtpSender?.close()
                rtpSender = null
                audioSender?.close()
                audioSender = null
            }

            cameraManager?.stopBackgroundThread()
            previewSurface = null

        } finally {
            isTearingDown = false
        }
    }

    fun stopStreaming() {
        // Guard against concurrent/double stop calls
        if (!isStreaming) {
            Log.i(TAG, "stopStreaming: already stopped, ignoring")
            stopForeground(STOP_FOREGROUND_REMOVE)

            // Don't stopSelf() here — let the caller decide lifecycle
            stopSelf()
            return
        }

        isStreaming = false  // ← set synchronously so double-press guard works immediately

        serviceScope.launch {
            teardown()
            // stopForeground/stopSelf are Service methods — must run on main thread
            withContext(Dispatchers.Main) {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
    }

    fun extractNalsFromAnnexB(buffer: ByteArray): List<ByteArray> {
        val nals              = mutableListOf<ByteArray>()
        val startCodeIndices  = mutableListOf<Int>()

        for (i in 0..buffer.size - 3) {
            if (buffer[i] == 0x00.toByte() && buffer[i + 1] == 0x00.toByte()) {
                if (buffer[i + 2] == 0x01.toByte()) {
                    startCodeIndices.add(i)
                } else if (i + 3 < buffer.size &&
                    buffer[i + 2] == 0x00.toByte() &&
                    buffer[i + 3] == 0x01.toByte()) {
                    startCodeIndices.add(i)
                }
            }
        }

        for (j in startCodeIndices.indices) {
            val startIndex      = startCodeIndices[j]
            val startCodeLength = if (startIndex + 3 < buffer.size &&
                buffer[startIndex + 2] == 0x00.toByte() &&
                buffer[startIndex + 3] == 0x01.toByte()) 4 else 3
            val nalDataStart    = startIndex + startCodeLength
            val endIndex        = if (j < startCodeIndices.size - 1)
                startCodeIndices[j + 1] else buffer.size
            if (endIndex > nalDataStart)
                nals.add(buffer.copyOfRange(nalDataStart, endIndex))
        }

        Log.i(TAG, "Nals size: ${nals.size}")

        return nals
    }

    private fun createNotification(contentText: String): Notification {
        val launchIntent = packageManager.getLaunchIntentForPackage(packageName)
        val contentPi = PendingIntent.getActivity(this, 0, launchIntent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stopPi = PendingIntent.getService(this, 0,
            Intent(this, CameraStreamService::class.java).apply { action = ACTION_STOP_STREAM },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Phone Webcam").setContentText(contentText)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(contentPi)
            .addAction(R.drawable.ic_stop, "Stop", stopPi)
            .setOngoing(true).build()
    }

    private fun updateNotification(contentText: String) {
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIFICATION_ID, createNotification(contentText))
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL_ID, CHANNEL_NAME,
                NotificationManager.IMPORTANCE_DEFAULT).apply {
                description = "Camera streaming"
                setShowBadge(false)
            }
            getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
        }
    }

    override fun onDestroy() {
        super.onDestroy()

        val scope = serviceScope
        serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default) // fresh scope for any restart

        scope.launch { teardown() }.invokeOnCompletion {
            scope.cancel()
        }
    }

    fun getStreamingStatus(): StreamStatus {
        val elapsed = (System.currentTimeMillis() - startTime) / 1000
        val fc      = frameCount.get()
        return StreamStatus(
            isStreaming = isStreaming,
            frameCount  = fc,
            bytesSent   = bytesSent.get(),
            fps         = if (isStreaming && elapsed > 0) fc / elapsed else 0,
            targetInfo  = "RTP active",
            elapsedSeconds = elapsed,
            bitrateMbps = bitrateMbps * 1_000_000
        )
    }

    data class StreamStatus(
        val isStreaming: Boolean,
        val frameCount: Long,
        val bytesSent: Long,
        val elapsedSeconds: Long,
        val fps: Long,
        val targetInfo: String,
        val bitrateMbps: Any
    )
}