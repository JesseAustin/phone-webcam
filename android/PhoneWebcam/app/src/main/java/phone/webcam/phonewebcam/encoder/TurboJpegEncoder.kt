package phone.webcam.phonewebcam.encoder

class TurboJpegEncoder {
    companion object {
        init { System.loadLibrary("phonewebcam_jni") }
    }
    external fun compressI420(
        yBuf: ByteArray,
        uBuf: ByteArray,
        vBuf: ByteArray,
        yRowStride: Int,
        uvRowStride: Int,
        uvPixelStride: Int,
        width: Int,
        height: Int,
        quality: Int
    ): ByteArray?
}