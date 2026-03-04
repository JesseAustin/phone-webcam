#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "srtp-session.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define closesocket close
#define SOCKET_ERROR (-1)
#endif

// ---------------------------------------------------------------------------
// RtpReceiver
//
// Receives H.264 over RTP (RFC 3984) as sent by RtpSender.kt.
// Supports:
//   - Single NAL unit packets  (NAL type 1–23)
//   - FU-A fragmented packets  (NAL type 28)
//
// Output of receive_frame() is a complete Annex-B bitstream
// (each NAL prefixed with 00 00 00 01) ready for avcodec_send_packet().
//
// The public interface is intentionally identical to the old UdpReceiver so
// phone-source.cpp requires no changes.
// ---------------------------------------------------------------------------
class RtpReceiver {
public:
    RtpReceiver();
    ~RtpReceiver();

    // Inject external SRTP session
    void set_srtp_session(SrtpSession* srtp) { srtp_ = srtp; }

    std::vector<uint8_t> pending_params_;  // buffered SPS+PPS
    //bool                 has_params_ = false;

    bool start(uint16_t port);
    void stop();

    // Block until a UDP packet arrives or timeout_ms elapses.
    // Returns true if data is available.
    bool wait_for_frame_ready(int timeout_ms);

    // Pull one RTP packet and attempt to complete a frame.
    // Returns true (and fills frame_data with Annex-B NAL data) when a
    // complete frame boundary is detected (marker bit set on last packet).
    bool receive_frame(std::vector<uint8_t>& frame_data);


private:
    socket_t socket_  = INVALID_SOCKET;
    bool initialized_ = false;

    // SRTP Session for encrypting the H.264 video stream:
    SrtpSession* srtp_ = nullptr;

    // -----------------------------------------------------------------------
    // FU-A reassembly state (one in-flight fragmented NAL at a time)
    // -----------------------------------------------------------------------
    struct FuState {
        uint16_t           start_seq  = 0;    // RTP seq of the first FU-A packet
        uint8_t            nal_header = 0;    // reconstructed NAL header byte
        uint16_t expected_seq = 0;
        std::vector<uint8_t> payload;         // accumulated FU payload bytes
        bool               active    = false;
    } fu_;

    // -----------------------------------------------------------------------
    // Frame accumulation — NALs collected until marker bit signals frame end
    // -----------------------------------------------------------------------
    std::vector<uint8_t> frame_buf_;  // Annex-B NALs accumulated for current frame

    // -----------------------------------------------------------------------
    // RTP state
    // -----------------------------------------------------------------------
    uint32_t last_ssrc_     = 0;
    uint16_t last_seq_      = 0;
    bool     first_packet_  = true;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    void appendAnnexB(const uint8_t* nal_data, size_t nal_size);
    void resetFu();    
};
