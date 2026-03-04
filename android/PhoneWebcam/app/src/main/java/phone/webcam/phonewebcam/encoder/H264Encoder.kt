package phone.webcam.phonewebcam.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.opengl.EGL14
import android.opengl.EGLContext
import android.opengl.GLES20
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

class H264Encoder(
    private val width: Int,
    private val height: Int,
    private val bitrateBps: Int = 4_000_000, // 20 Mbps
    private val frameRate: Int = 30
) {
    companion object {
        private const val TAG = "PhoneWebcam"
        private const val MIME_TYPE = "video/avc"
        private const val TIMEOUT_US = 10_000L
    }

    private var codec: MediaCodec? = null
    private var _inputSurface: Surface? = null
    val inputSurface: Surface get() = _inputSurface!!

    var cachedSps: ByteArray? = null
    var cachedPps: ByteArray? = null

    // Called for each encoded NAL
    var onEncodedFrame: ((ByteArray, Long, Boolean) -> Unit)? = null

    fun start() {
        // Clear cached SPS/PPS metadata before starting a new stream
        cachedSps = null;
        cachedPps = null;

        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
            setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
            setInteger(MediaFormat.KEY_LATENCY, 0)
            setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel31)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 0)
            setInteger(MediaFormat.KEY_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)

            // Use variable bitrate mode
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            // Constantly prepend SPS and PPS to each frame
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
        }

        codec = MediaCodec.createEncoderByType(MIME_TYPE).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            _inputSurface = createInputSurface() // GPU-render target
            start()
        }
        Log.i(TAG, "H264Encoder started ${width}x${height} @ ${bitrateBps/1000}kbps")
    }

    /**
     * Drain encoded frames — same as before
     */
    fun drainOutput() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()

        while (true) {
            val idx = try {
                c.dequeueOutputBuffer(info, TIMEOUT_US)
            } catch (e: IllegalStateException) {
                // This can happen if the codec is stopped or released.
                return
            }
            when {
                idx == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val format = c.outputFormat
                    cachedSps = format.getByteBuffer("csd-0")?.let { ByteArray(it.remaining()).also { b -> it.get(b) } }
                    cachedPps = format.getByteBuffer("csd-1")?.let { ByteArray(it.remaining()).also { b -> it.get(b) } }

                    cachedSps?.let { onEncodedFrame?.invoke(it, 0, false) }
                    cachedPps?.let { onEncodedFrame?.invoke(it, 0, false) }
                }
                idx >= 0 -> {
                    val buf = c.getOutputBuffer(idx) ?: run { c.releaseOutputBuffer(idx, false); continue }

                    if (info.size > 0 && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        val bytes = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        buf.get(bytes)

                        // --- NAL SPLITTING LOGIC ---
                        val nals = mutableListOf<ByteArray>()

                        var i = 0
                        var nalStart = -1

                        while (i <= bytes.size - 4) {
                            val isFourByteStart = bytes[i] == 0.toByte() && bytes[i+1] == 0.toByte() &&
                                    bytes[i+2] == 0.toByte() && bytes[i+3] == 1.toByte()

                            val isThreeByteStart = !isFourByteStart &&
                                    bytes[i] == 0.toByte() && bytes[i+1] == 0.toByte() && bytes[i+2] == 1.toByte()

                            if (isFourByteStart || isThreeByteStart) {
                                if (nalStart != -1) {
                                    nals.add(bytes.copyOfRange(nalStart, i))
                                }
                                nalStart = i
                                i += if (isFourByteStart) 4 else 3
                            } else {
                                i++
                            }
                        }

                        // Send the very last NAL in the buffer
                        if (nalStart != -1) {
                            nals.add(bytes.copyOfRange(nalStart, bytes.size))
                        }
                        // ---------------------------

                        val sliceIndices = nals.mapIndexedNotNull { index, nal ->
                            val type = nal.dropWhile { it == 0.toByte() }.firstOrNull()?.toInt()?.and(0x1F)
                            if (type != null && type in 1..5) index else null
                        }

                        val lastSliceIndex = sliceIndices.lastOrNull()

                        nals.forEachIndexed { index, nal ->
                            val isLast = index == lastSliceIndex
                            onEncodedFrame?.invoke(nal, info.presentationTimeUs, isLast)
                        }
                    }
                    c.releaseOutputBuffer(idx, false)
                }
            }
        }
    }

    fun stop() {
        try {
            // Stop the codec immediately for live streams
            codec?.stop()
        } catch (e: Exception) {
            Log.e(TAG, "Codec stop failed (normal if already stopped)")
        } finally {
            try {
                codec?.release()
            } catch (e: Exception) {}

            _inputSurface?.release()
            _inputSurface = null
            codec = null
        }
        Log.i(TAG, "H264Encoder stopped")
    }

    fun requestKeyFrame() {
        val params = android.os.Bundle().apply {
            putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
        }
        codec?.setParameters(params)
    }
}
