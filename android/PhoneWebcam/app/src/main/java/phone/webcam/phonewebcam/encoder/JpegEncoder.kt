package phone.webcam.phonewebcam.encoder

import android.media.Image
import android.util.Log

class JpegEncoder {

    companion object {
        private const val TAG = "JpegEncoder"
        const val QUALITY_HIGH = 85
    }

    private val turbo = TurboJpegEncoder()

    fun encode(image: Image, quality: Int = QUALITY_HIGH): ByteArray? {

        //Debugging log to check if quality slider is working properly:
        //Log.d("JpegEncoder", "Encoding at quality $quality")

        return try {
            val yPlane = image.planes[0]
            val uPlane = image.planes[1]
            val vPlane = image.planes[2]

            val yBuf = yPlane.buffer.let { buf ->
                val arr = ByteArray(buf.remaining()); buf.get(arr); arr
            }
            val uBuf = uPlane.buffer.let { buf ->
                val arr = ByteArray(buf.remaining()); buf.get(arr); arr
            }
            val vBuf = vPlane.buffer.let { buf ->
                val arr = ByteArray(buf.remaining()); buf.get(arr); arr
            }

            turbo.compressI420(
                yBuf, uBuf, vBuf,
                yPlane.rowStride,
                uPlane.rowStride,
                uPlane.pixelStride,
                image.width,
                image.height,
                quality
            )
        } catch (e: Exception) {
            Log.e(TAG, "JPEG encoding failed", e)
            null
        }
    }
}
