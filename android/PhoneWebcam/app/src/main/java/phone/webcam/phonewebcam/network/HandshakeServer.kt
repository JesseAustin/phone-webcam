package phone.webcam.phonewebcam.network

import android.util.Log
import kotlinx.coroutines.*
import org.json.JSONObject
import phone.webcam.phonewebcam.security.HmacAuth
import java.net.ServerSocket
import java.net.Socket

/**
 * Listens for the OBS handshake on port 9001.
 *
 * Auth protocol (when password is set on both sides):
 *
 *   OBS → Android:  {"ip":"…","port":…,"challenge":"<hex32>","hmac":"<hex32>"}
 *
 *   1. Android verifies OBS's HMAC:  HMAC(password, challenge) == hmac
 *      → If wrong: drop silently (wrong password or wrong OBS instance)
 *   2. Android replies with its own HMAC so OBS can also verify us:
 *      (Not implemented in OBS server here, but the structure is ready)
 *
 * If no password is configured, the challenge/hmac fields are absent and
 * the connection proceeds exactly as before — fully backward compatible.
 */
class HandshakeServer(
    private val onHandshakeReceived: (ip: String, port: Int) -> Unit
) {
    companion object {
        private const val TAG = "PhoneWebcam"
        const val HANDSHAKE_PORT = 9001
    }

    private var serverSocket: ServerSocket? = null
    private var scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // Password — updated live from StreamViewModel
    @Volatile var password: String = ""

    fun start() {
        scope.launch {
            try {
                serverSocket = ServerSocket().apply {
                    reuseAddress = true
                    bind(java.net.InetSocketAddress(HANDSHAKE_PORT))
                }
                Log.i(TAG, "Handshake server listening on port $HANDSHAKE_PORT")

                while (isActive) {
                    val client: Socket = serverSocket?.accept() ?: break
                    Log.i(TAG, "Handshake connection from ${client.inetAddress.hostAddress}")
                    handleClient(client)
                }
            } catch (e: Exception) {
                if (isActive) Log.e(TAG, "Handshake server error: ${e.message}")
            }
        }
    }

    private fun handleClient(client: Socket) {
        try {
            val message = client.inputStream
                .bufferedReader()
                .readLine() ?: return

            Log.i(TAG, "Handshake received: $message")

            val json = JSONObject(message)
            val ip   = json.getString("ip")
            val port = json.getInt("port")

            val ts = json.optLong("ts", -1L)
            if (ts > 0) {
                val now = System.currentTimeMillis() / 1000
                val diff = now - ts
                if (diff < -5 || diff > 30) {
                    Log.w(TAG, "Handshake rejected — timestamp stale (${diff}s)")
                    return
                }
            }

            // ------------------------------------------------------------------
            // Auth check
            //
            // Rules:
            //   - Android has password, OBS sent token  → verify HMAC
            //   - Android has password, OBS sent no token → reject (OBS has no password)
            //   - Android no password, OBS sent token   → reject (OBS has password, we don't)
            //   - Android no password, OBS sent no token → accept (both passwordless)
            // ------------------------------------------------------------------
            val currentPassword = password  // snapshot — @Volatile ensures visibility
            val challengeHex = json.optString("challenge", "")
            val receivedHmac = json.optString("hmac", "")
            val obsHasPassword = challengeHex.isNotEmpty() && receivedHmac.isNotEmpty()

            if (currentPassword.isNotEmpty() && obsHasPassword) {
                // Both sides have passwords — verify the HMAC
                val challengeBytes = HmacAuth.fromHex(challengeHex)
                if (challengeBytes == null) {
                    Log.w(TAG, "Handshake: rejected — invalid challenge hex")
                    return
                }
                if (!HmacAuth.verifyHmac(currentPassword, challengeBytes, receivedHmac)) {
                    Log.w(TAG, "Handshake: rejected — HMAC mismatch (wrong password)")
                    return
                }
                Log.i(TAG, "Handshake: auth OK ✓")

            } else if (currentPassword.isNotEmpty() && !obsHasPassword) {
                // Android has password but OBS sent none — reject
                Log.w(TAG, "Handshake: rejected — Android has password but OBS sent no token")
                return

            } else if (currentPassword.isEmpty() && obsHasPassword) {
                // OBS has password but Android has none — reject
                Log.w(TAG, "Handshake: rejected — OBS has password but Android has none")
                return

            } else {
                // Both passwordless — accept
                Log.i(TAG, "Handshake: no auth (both sides passwordless) — accepted")
            }
            // Auth passed (or no password) — notify ViewModel
            onHandshakeReceived(ip, port)

        } catch (e: Exception) {
            Log.e(TAG, "Handshake parse error: ${e.message}")
        } finally {
            client.close()
        }
    }

    fun stop() {
        scope.cancel()
        try { serverSocket?.close() } catch (e: Exception) { }
        serverSocket = null
        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    }
}