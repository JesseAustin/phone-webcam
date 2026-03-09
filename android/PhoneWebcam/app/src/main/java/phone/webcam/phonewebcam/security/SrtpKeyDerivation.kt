package phone.webcam.phonewebcam.security

import android.util.Log
import java.security.spec.KeySpec
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec

object SrtpKeyDerivation {

    /**
     * Derives a 30-byte SRTP key (16 bytes key + 14 bytes salt) 
     * from a user password using PBKDF2.
     */
    fun deriveKeys(password: String, salt: ByteArray): Pair<ByteArray, ByteArray> {
        val factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
        // 30 bytes = 240 bits
        val spec: KeySpec = PBEKeySpec(password.toCharArray(), salt, 7000, 240)
        val tmp = factory.generateSecret(spec)
        val fullKey = tmp.encoded

        val masterKey = fullKey.copyOfRange(0, 16)
        val masterSalt = fullKey.copyOfRange(16, 30)

        return Pair(masterKey, masterSalt)
    }
}