package phone.webcam.phonewebcam.network

import android.util.Log

class SrtpContext : AutoCloseable {
    companion object {
        init {
            System.loadLibrary("phonewebcam_jni")
        }
    }

    /**
     * Initializes the SRTP session with a 128-bit master key and 112-bit salt.
     * @param masterKey 16 bytes
     * @param masterSalt 14 bytes
     * @return 0 on success, or a libsrtp error code
     */
    external fun init(masterKey: ByteArray, masterSalt: ByteArray, ssrc: Int): Int

    /**
     * Encrypts an RTP packet in-place.
     * @param packet The byte array containing the RTP packet. 
     *               MUST have at least 10 bytes of extra space at the end for the auth tag.
     * @param length The length of the original RTP packet (header + payload).
     * @return The new length of the encrypted packet (usually length + 10), or negative on error.
     */
    external fun protect(packet: ByteArray, length: Int): Int

    private external fun closeNative()

    override fun close() {
        closeNative()
    }
}