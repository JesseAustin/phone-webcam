package phone.webcam.phonewebcam.network

import android.util.Log
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.random.Random

class RtpSender(
    private val host: String,
    private val port: Int,
    masterKey: ByteArray? = null,
    masterSalt: ByteArray? = null
) {

    private val DEBUG_SKIP_SRTP = false  // set to true to test unencrypted

    private val socket = DatagramSocket()
    private val address = InetAddress.getByName(host)

    private var sequenceNumber = 0
    private val ssrc = Random.nextInt()

    private val MAX_PAYLOAD = 1200
    
    // SRTP Context
    private var srtpContext: SrtpContext? = null
    
    init {
        if (masterKey != null && masterSalt != null) {
            srtpContext = SrtpContext().apply {
                val res = init(masterKey, masterSalt, ssrc)
                if (res != 0) {
                    Log.e("RtpSender", "Failed to initialize SRTP: $res")
                } else {
                    Log.i("RtpSender", "SRTP initialized successfully")
                }
            }
        }
    }

    /**
     * Sends a generic RTP packet. Useful for audio or non-NAL payloads.
     */
    fun sendRtp(payload: ByteArray, rtpTimestamp: Int, payloadType: Int, marker: Boolean = false) {
        if (payload.isEmpty()) return
        try {
            val rtpLen = 12 + payload.size
            val packet = ByteArray(rtpLen + 10) // 12 (RTP) + payload + 10 (SRTP tag)

            // RTP header
            packet[0] = 0x80.toByte() // V=2
            packet[1] = ((if (marker) 0x80 else 0x00) or (payloadType and 0x7F)).toByte()
            packet[2] = (sequenceNumber shr 8).toByte()
            packet[3] = (sequenceNumber and 0xFF).toByte()
            packet[4] = (rtpTimestamp shr 24).toByte()
            packet[5] = (rtpTimestamp shr 16).toByte()
            packet[6] = (rtpTimestamp shr 8).toByte()
            packet[7] = (rtpTimestamp and 0xFF).toByte()
            packet[8] = (ssrc shr 24).toByte()
            packet[9] = (ssrc shr 16).toByte()
            packet[10] = (ssrc shr 8).toByte()
            packet[11] = (ssrc and 0xFF).toByte()

            System.arraycopy(payload, 0, packet, 12, payload.size)
            
            sendMaybeEncrypted(packet, rtpLen)
            sequenceNumber++
        } catch (e: IOException) {
            Log.d("RtpSender", "Send skipped: ${e.message}")
        }
    }

    fun reset() {
        sequenceNumber = 0
    }

    fun sendNal(nal: ByteArray, rtpTimestamp: Int, isLastNalOfFrame: Boolean) {
        if (nal.isEmpty()) return

        val cleanNal = stripStartCode(nal)
        if (cleanNal.isEmpty()) return

        val nalType = cleanNal[0].toInt() and 0x1F
        val isSlice = nalType in 1..5
        val marker = isSlice && isLastNalOfFrame

        if (cleanNal.size <= MAX_PAYLOAD) {
            sendSingle(cleanNal, rtpTimestamp, marker)
        } else {
            sendFragmented(cleanNal, rtpTimestamp, marker)
        }
    }

    private fun stripStartCode(nal: ByteArray): ByteArray {
        if (nal.size >= 4 &&
            nal[0] == 0.toByte() &&
            nal[1] == 0.toByte() &&
            nal[2] == 0.toByte() &&
            nal[3] == 1.toByte()
        ) {
            return nal.copyOfRange(4, nal.size)
        }

        if (nal.size >= 3 &&
            nal[0] == 0.toByte() &&
            nal[1] == 0.toByte() &&
            nal[2] == 1.toByte()
        ) {
            return nal.copyOfRange(3, nal.size)
        }

        return nal
    }

    fun close() {
        srtpContext?.close()
        socket.close()
    }

    private fun sendSingle(nal: ByteArray, timestamp: Int, marker: Boolean) {
        if (nal.isEmpty()) return
        try {
            // Allocate 12 (RTP) + NAL + 10 (SRTP Auth Tag)
            val rtpLen = 12 + nal.size
            val packet = ByteArray(rtpLen + 10)

            // RTP header
            packet[0] = 0x80.toByte() // V=2
            packet[1] = ((if (marker) 0x80 else 0x00) or 96).toByte()
            packet[2] = (sequenceNumber shr 8).toByte()
            packet[3] = (sequenceNumber and 0xFF).toByte()
            packet[4] = (timestamp shr 24).toByte()
            packet[5] = (timestamp shr 16).toByte()
            packet[6] = (timestamp shr 8).toByte()
            packet[7] = (timestamp and 0xFF).toByte()
            packet[8] = (ssrc shr 24).toByte()
            packet[9] = (ssrc shr 16).toByte()
            packet[10] = (ssrc shr 8).toByte()
            packet[11] = (ssrc and 0xFF).toByte()

            System.arraycopy(nal, 0, packet, 12, nal.size)
            
            sendMaybeEncrypted(packet, rtpLen)
            sequenceNumber++
        } catch (e: IOException) {
            Log.d("RtpSender", "Send skipped: ${e.message}")
        }
    }

    private fun sendFragmented(nal: ByteArray, timestamp: Int, nalMarker: Boolean) {
        if (nal.isEmpty()) return
        val nalHeader = nal[0]
        val nri = nalHeader.toInt() and 0x60

        var offset = 1
        var first = true

        while (offset < nal.size) {
            val remaining = nal.size - offset
            val chunkSize = minOf(MAX_PAYLOAD - 2, remaining)
            
            // 12 (RTP) + 2 (FU) + Chunk + 10 (SRTP)
            val rtpLen = 12 + 2 + chunkSize
            val packet = ByteArray(rtpLen + 10)

            packet[0] = 0x80.toByte()
            val isLastFragment = offset + chunkSize >= nal.size
            val marker = isLastFragment && nalMarker
            packet[1] = ((if (marker) 0x80 else 0x00) or 96).toByte()
            packet[2] = (sequenceNumber shr 8).toByte()
            packet[3] = (sequenceNumber and 0xFF).toByte()
            packet[4] = (timestamp shr 24).toByte()
            packet[5] = (timestamp shr 16).toByte()
            packet[6] = (timestamp shr 8).toByte()
            packet[7] = (timestamp and 0xFF).toByte()
            packet[8] = (ssrc shr 24).toByte()
            packet[9] = (ssrc shr 16).toByte()
            packet[10] = (ssrc shr 8).toByte()
            packet[11] = (ssrc and 0xFF).toByte()

            packet[12] = (nri or 28).toByte() // FU indicator
            var fuHeader = (nalHeader.toInt() and 0x1F)
            if (first) fuHeader = fuHeader or 0x80
            if (isLastFragment) fuHeader = fuHeader or 0x40
            packet[13] = fuHeader.toByte()

            System.arraycopy(nal, offset, packet, 14, chunkSize)

            sendMaybeEncrypted(packet, rtpLen)
            sequenceNumber++

            offset += chunkSize
            first = false
        }
    }

    private fun sendMaybeEncrypted(packet: ByteArray, rtpLength: Int) {
        val finalLen = if (DEBUG_SKIP_SRTP) {
            rtpLength
        } else {
            srtpContext?.protect(packet, rtpLength) ?: rtpLength
        }


        //Log.d("RtpSender", "SRTP: rtpLen=$rtpLength finalLen=$finalLen encrypted=${srtpContext != null}")

        if (finalLen > 0) {
            socket.send(DatagramPacket(packet, finalLen, address, port))
        }
    }
}
