package phone.webcam.phonewebcam.security

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * HMAC-SHA256 authentication — mirrors auth.h on the OBS side.
 *
 * Both sides must compute:
 *   HMAC-SHA256( UTF-8(password), challenge_bytes )
 * and encode the result as lowercase hex.
 *
 * If both sides know the same password, the MACs match.
 */
object HmacAuth {

    /**
     * Compute HMAC-SHA256(password, challengeBytes) → lowercase hex string.
     * Returns null if password is blank (no auth).
     */
    fun computeHmac(password: String, challengeBytes: ByteArray): String? {
        if (password.isEmpty()) return null

        val mac = Mac.getInstance("HmacSHA256")
        val keySpec = SecretKeySpec(password.toByteArray(Charsets.UTF_8), "HmacSHA256")
        mac.init(keySpec)
        val result = mac.doFinal(challengeBytes)
        return result.toHex()
    }

    /**
     * Verify that receivedHmac matches HMAC-SHA256(password, challengeBytes).
     * Uses constant-time comparison to prevent timing attacks.
     */
    fun verifyHmac(password: String, challengeBytes: ByteArray, receivedHmac: String): Boolean {
        val expected = computeHmac(password, challengeBytes) ?: return false
        return constantTimeEqual(expected, receivedHmac.lowercase())
    }

    /**
     * Decode a lowercase hex string to a ByteArray.
     * Returns null if the string is not valid hex.
     */
    fun fromHex(hex: String): ByteArray? {
        if (hex.length % 2 != 0) return null
        return try {
            ByteArray(hex.length / 2) { i ->
                hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
        } catch (e: NumberFormatException) {
            null
        }
    }

    private fun ByteArray.toHex(): String =
        joinToString("") { "%02x".format(it) }

    /** Constant-time string comparison (same length only) */
    private fun constantTimeEqual(a: String, b: String): Boolean {
        if (a.length != b.length) return false
        var diff = 0
        for (i in a.indices) diff = diff or (a[i].code xor b[i].code)
        return diff == 0
    }
}