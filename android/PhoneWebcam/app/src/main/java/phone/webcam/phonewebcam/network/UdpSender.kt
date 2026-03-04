package phone.webcam.phonewebcam.network

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet6Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Sends UDP packets to the OBS plugin.
 *
 * IPv6 / IPv4 dual-stack strategy:
 *
 * InetAddress.getByName() already handles both "192.168.1.5" and
 * "2001:db8::1" transparently — it returns the right subtype.
 * The trick is that Java's DatagramSocket must be *bound* to the matching
 * address family before it can send to an IPv6 address; a default-constructed
 * DatagramSocket binds to an IPv4 wildcard and will throw on Inet6Address.
 *
 * Fix: inspect the resolved address type and bind the socket to the
 * matching wildcard ("::" for IPv6, "0.0.0.0" for IPv4).
 *
 * The OBS plugin receiver is a dual-stack AF_INET6 socket, so it accepts
 * both — the user just types whatever IP their network shows.
 */
class UdpSender(
    private val targetIp: String,
    private val targetPort: Int
) {
    companion object {
        private const val TAG = "PhoneWebcam"
        const val MAX_DATA_SIZE   = 1400
        const val HEADER_SIZE     = 16
        private const val MAX_PACKET_SIZE = HEADER_SIZE + MAX_DATA_SIZE
    }

    private var socket: DatagramSocket? = null
    private var targetAddress: InetAddress? = null
    @Volatile var isInitialized = false
        private set

    // Pre-allocated send buffer — zero heap allocation in hot path
    private val packetBuf  = ByteArray(MAX_PACKET_SIZE)
    private val headerView = ByteBuffer.wrap(packetBuf).order(ByteOrder.LITTLE_ENDIAN)
    private val datagram   = DatagramPacket(packetBuf, 0)

    fun initialize(): Result<Unit> {
        return try {
            val addr = InetAddress.getByName(targetIp)
            targetAddress = addr

            // Bind the socket to the correct address family.
            // A plain DatagramSocket() binds to IPv4; it cannot send to an
            // Inet6Address. We must explicitly bind to the IPv6 wildcard for
            // IPv6 destinations.
            val bindAddr: InetAddress = if (addr is Inet6Address) {
                Log.i(TAG, "Target is IPv6 — binding socket to IPv6 wildcard (::)")
                InetAddress.getByName("::")       // IPv6 wildcard
            } else {
                Log.i(TAG, "Target is IPv4 — binding socket to IPv4 wildcard (0.0.0.0)")
                InetAddress.getByName("0.0.0.0")  // IPv4 wildcard
            }

            socket = DatagramSocket(InetSocketAddress(bindAddr, 0)).apply {
                sendBufferSize = 512 * 1024
            }

            isInitialized = true
            val family = if (addr is Inet6Address) "IPv6" else "IPv4"
            Log.i(TAG, "UDP sender ($family) → $targetIp:$targetPort")
            Result.success(Unit)

        } catch (e: Exception) {
            Log.e(TAG, "Init failed", e)
            Result.failure(e)
        }
    }

    /**
     * Send one complete JPEG frame as UDP packets. Blocking.
     * Must be called from a single dedicated thread — not concurrency-safe.
     * Zero heap allocation in the hot path.
     */
    fun sendFrame(jpegData: ByteArray, sequenceNumber: Int): Boolean {
        val sock    = socket        ?: return false
        val address = targetAddress ?: return false

        val totalPackets = (jpegData.size + MAX_DATA_SIZE - 1) / MAX_DATA_SIZE

        return try {
            for (i in 0 until totalPackets) {
                val offset  = i * MAX_DATA_SIZE
                val dataLen = minOf(MAX_DATA_SIZE, jpegData.size - offset)

                // Write header into pre-allocated buffer (absolute positions,
                // no need to flip/clear a relative ByteBuffer)
                headerView.putInt(0,  sequenceNumber)
                headerView.putInt(4,  totalPackets)
                headerView.putInt(8,  i)
                headerView.putInt(12, MAX_DATA_SIZE) // always constant stride

                System.arraycopy(jpegData, offset, packetBuf, HEADER_SIZE, dataLen)

                datagram.address = address
                datagram.port    = targetPort
                datagram.length  = HEADER_SIZE + dataLen
                sock.send(datagram)
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "Send error frame #$sequenceNumber", e)
            false
        }
    }

    fun close() {
        socket?.close()
        socket = null
        isInitialized = false
    }

    fun getTargetInfo(): String {
        val addr = targetAddress
        val family = if (addr is Inet6Address) "IPv6" else "IPv4"
        return "[$family] $targetIp:$targetPort"
    }
}
