package phone.webcam.phonewebcam.security

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * Manages the stream password used for HMAC-SHA256 authentication.
 *
 * Stores the password in EncryptedSharedPreferences (AES-256 at rest).
 * Falls back to plain SharedPreferences on devices/emulators where
 * the security library isn't available, so development is never blocked.
 *
 * Usage:
 *   PasswordManager.setPassword(context, "mySecret")
 *   val pw = PasswordManager.getPassword(context)  // "" if not set
 */
object PasswordManager {

    private const val PREFS_FILE  = "phonewebcam_secure_prefs"
    private const val KEY_PASSWORD = "stream_password"

    fun setPassword(context: Context, password: String) {
        getPrefs(context).edit().putString(KEY_PASSWORD, password).apply()
    }

    fun getPassword(context: Context): String {
        return getPrefs(context).getString(KEY_PASSWORD, "") ?: ""
    }

    fun clearPassword(context: Context) {
        getPrefs(context).edit().remove(KEY_PASSWORD).apply()
    }

    // -----------------------------------------------------------------------
    // Try EncryptedSharedPreferences first; gracefully degrade if unavailable
    // -----------------------------------------------------------------------
    private fun getPrefs(context: Context): SharedPreferences {
        return try {
            val masterKey = MasterKey.Builder(context)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
                .build()

            EncryptedSharedPreferences.create(
                context,
                PREFS_FILE,
                masterKey,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
            )
        } catch (e: Exception) {
            // Fallback for emulators or devices without hardware-backed keystore
            android.util.Log.w("PasswordManager",
                "EncryptedSharedPreferences unavailable, using plain prefs: ${e.message}")
            context.getSharedPreferences(PREFS_FILE + "_plain", Context.MODE_PRIVATE)
        }
    }
}