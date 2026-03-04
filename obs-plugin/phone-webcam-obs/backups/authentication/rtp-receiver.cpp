#define NOMINMAX

#include "rtp-receiver.h"
#include <obs-module.h>
#include <cstring>
#include <algorithm>


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

// ---------------------------------------------------------------------------
// Annex-B start code
// ---------------------------------------------------------------------------
static const uint8_t kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

// ---------------------------------------------------------------------------
// RTP header layout (12 bytes, big-endian)
// ---------------------------------------------------------------------------
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

static constexpr int RTP_HDR_SIZE = 12;

// ---------------------------------------------------------------------------
RtpReceiver::RtpReceiver()
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        blog(LOG_ERROR, "RtpReceiver: WSAStartup failed");
        return;
    }
#endif
    initialized_ = true;
}

RtpReceiver::~RtpReceiver()
{
    stop();
#ifdef _WIN32
    if (initialized_)
        WSACleanup();
#endif
}


// ---------------------------------------------------------------------------
bool RtpReceiver::start(uint16_t port)
{

    if (!initialized_) return false;
    stop();

    // Dual-stack IPv6 socket (accepts IPv4-mapped addresses too)
    socket_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        blog(LOG_ERROR, "RtpReceiver: socket() failed");
        return false;
    }

        int v6only = 0;
    #ifdef _WIN32
        setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
    #else
        setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    #endif

        int reuse = 1;
    #ifdef _WIN32
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    #else
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    #endif

        int rcvbuf = 16 * 1024 * 1024;
    #ifdef _WIN32
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    #else
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    #endif

        // Non-blocking
    #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(socket_, FIONBIO, &mode);
    #else
        int fl = fcntl(socket_, F_GETFL, 0);
        fcntl(socket_, F_SETFL, fl | O_NONBLOCK);
    #endif

        sockaddr_in6 addr = {};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr   = in6addr_any;
        addr.sin6_port   = htons(port);

        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    #ifdef _WIN32
            blog(LOG_ERROR, "RtpReceiver: bind failed on port %d, error %d", port, WSAGetLastError());
    #else
            blog(LOG_ERROR, "RtpReceiver: bind failed on port %d", port);
    #endif
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }


        // Flush any stale packets from previous session
        uint8_t flush_buf[2048];
        sockaddr_storage flush_sender{};
        #ifdef _WIN32
        int flush_len = sizeof(flush_sender);
        #else
        socklen_t flush_len = sizeof(flush_sender);
        #endif
        while (recvfrom(socket_, (char*)flush_buf, sizeof(flush_buf), 0,
                        reinterpret_cast<sockaddr*>(&flush_sender), &flush_len) > 0) {}

        // Reset state
        first_packet_ = true;
        pending_params_.clear();
        frame_buf_.clear();
        resetFu();

        blog(LOG_INFO, "UDP receiver started on port %d (dual-stack IPv4+IPv6)", port);
        return true;
}

// ---------------------------------------------------------------------------
void RtpReceiver::stop()
{
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

// ---------------------------------------------------------------------------
bool RtpReceiver::wait_for_frame_ready(int timeout_ms)
{
    if (socket_ == INVALID_SOCKET) return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket_, &fds);

    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    return select(static_cast<int>(socket_) + 1, &fds, nullptr, nullptr, &tv) > 0;
}

// ---------------------------------------------------------------------------
void RtpReceiver::appendAnnexB(const uint8_t* nal_data, size_t nal_size)
{
    // Prepend 00 00 00 01 start code
    frame_buf_.insert(frame_buf_.end(), kStartCode, kStartCode + 4);
    frame_buf_.insert(frame_buf_.end(), nal_data, nal_data + nal_size);
}

void RtpReceiver::resetFu()
{
    fu_.active     = false;
    fu_.nal_header = 0;
    fu_.start_seq  = 0;
    fu_.payload.clear();
}

// ---------------------------------------------------------------------------
// Main receive function — call in a loop, returns true when a full frame is
// ready (Annex-B bitstream in frame_data).
// ---------------------------------------------------------------------------
bool RtpReceiver::receive_frame(std::vector<uint8_t>& frame_data)
{
    if (socket_ == INVALID_SOCKET) return false;

    bool produced_frame = false;

    while (true) {
        uint8_t buf[2048];
        sockaddr_storage sender{};
    #ifdef _WIN32
        int sender_len = sizeof(sender);
    #else
        socklen_t sender_len = sizeof(sender);
    #endif

        int received = recvfrom(socket_, (char*)buf, sizeof(buf), 0,
                                reinterpret_cast<sockaddr*>(&sender), &sender_len);

        if (received <= 0) break; // no more packets available
        if (received < RTP_HDR_SIZE) continue;

        const uint8_t version = (buf[0] >> 6) & 0x03;
        if (version != 2) continue;

        const bool marker = (buf[1] & 0x80) != 0;
        const uint16_t seq = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];

        const uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                                (static_cast<uint32_t>(buf[9]) << 16) |
                                (static_cast<uint32_t>(buf[10]) << 8) |
                                static_cast<uint32_t>(buf[11]);

        if (!first_packet_ && ssrc != last_ssrc_) {
            frame_buf_.clear();
            pending_params_.clear();
            resetFu();
        }

        last_ssrc_ = ssrc;
        first_packet_ = false;
        last_seq_ = seq;

        const uint8_t* payload = buf + RTP_HDR_SIZE;
        int payload_len = received - RTP_HDR_SIZE;
        if (payload_len <= 0) continue;

        // RTP payload for H.264 never has Annex-B start codes.
        // nal_type is always at payload[0] for single NAL and STAP-A/FU-A indicator.
        int nal_type = payload[0] & 0x1F;

        // --- SPS/PPS buffering ---
        if (nal_type == 7 || nal_type == 8) {
            pending_params_.insert(pending_params_.end(), kStartCode, kStartCode + 4);
            pending_params_.insert(pending_params_.end(), payload, payload + payload_len);
            continue;
        }

        // --- Single NAL ---
        if (nal_type >= 1 && nal_type <= 23) {
            if (fu_.active) resetFu();
            if (nal_type == 5 && !pending_params_.empty()) {
                frame_buf_.insert(frame_buf_.end(), pending_params_.begin(), pending_params_.end());
            }
            appendAnnexB(payload, payload_len);
        }
        // --- FU-A ---
        else if (nal_type == 28) {
            if (payload_len < 2) continue;
            const uint8_t fu_indicator = payload[0];
            const uint8_t fu_header    = payload[1];
            const bool fu_start        = fu_header & 0x80;
            const bool fu_end          = fu_header & 0x40;
            const uint8_t fu_nal_type  = fu_header & 0x1F;
            const uint8_t* fu_payload  = payload + 2;
            int fu_payload_len         = payload_len - 2;

            //blog(LOG_INFO, "RTP: seq=%u marker=%d nal_type=%d payload_len=%d fu_start=%d fu_end=%d",
            //    seq, (int)marker, nal_type, payload_len, (int)fu_start, (int)fu_end);

            if (fu_start) {
                // If we were accumulating a previous frame, flush it now
                if (!frame_buf_.empty()) {
                    frame_data.swap(frame_buf_);
                    frame_buf_.clear();
                    produced_frame = true;
                }
                resetFu();
                fu_.active       = true;
                fu_.start_seq    = seq;
                fu_.expected_seq = seq + 1;
                fu_.nal_header   = (fu_indicator & 0x60) | fu_nal_type;
                fu_.payload.clear();
                fu_.payload.push_back(fu_.nal_header);
                fu_.payload.insert(fu_.payload.end(), fu_payload, fu_payload + fu_payload_len);
            } else if (fu_.active) {
                if ((uint16_t)(seq - fu_.expected_seq) != 0) {
                    resetFu();
                    continue;
                }
                fu_.expected_seq = seq + 1;
                fu_.payload.insert(fu_.payload.end(), fu_payload, fu_payload + fu_payload_len);
            } else continue;

            if (fu_end && fu_.active) {
                if ((fu_.nal_header & 0x1F) == 5 && !pending_params_.empty()) {
                    frame_buf_.insert(frame_buf_.end(), pending_params_.begin(), pending_params_.end());
                }
                appendAnnexB(fu_.payload.data(), fu_.payload.size());
                resetFu();
            }
            blog(LOG_INFO, "RTP: seq=%u marker=%d nal_type=%d payload_len=%d fu_start=%d fu_end=%d%s",
            seq, (int)marker, nal_type, payload_len, (int)fu_start, (int)fu_end,
            fu_end ? " [NAL complete]" : "");
        }
        
        // --- STAP-A ---
        else if (nal_type == 24) {
            const uint8_t* p = payload + 1;
            const uint8_t* end = payload + payload_len;
            while (p + 2 <= end) {
                uint16_t nal_size = (p[0] << 8) | p[1];
                p += 2;
                if (p + nal_size > end) break;
                appendAnnexB(p, nal_size);
                p += nal_size;
            }
        }
        // --- unknown ---
        else continue;

        // --- check marker bit ---
        if (marker && !frame_buf_.empty()) {
            frame_data.swap(frame_buf_);
            frame_buf_.clear();
            produced_frame = true;
        }
    }

    return produced_frame;
}
