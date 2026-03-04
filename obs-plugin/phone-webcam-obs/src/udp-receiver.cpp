#define NOMINMAX

#include "udp-receiver.h"
#include <obs-module.h>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif


UdpReceiver::UdpReceiver()
	: socket_(INVALID_SOCKET)
	, initialized_(false)
	//, current_sequence_(0)
	//, packets_received_(0)
	//, total_packets_(0)
{
#ifdef _WIN32
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		blog(LOG_ERROR, "WSAStartup failed");
		return;
	}
#endif
	initialized_ = true;
}

UdpReceiver::~UdpReceiver()
{
	stop();
#ifdef _WIN32
	if (initialized_)
		WSACleanup();
#endif
}

bool UdpReceiver::start(uint16_t port)
{
	if (!initialized_)
		return false;

	stop(); // close any previously open socket

	// -----------------------------------------------------------------------
	// Create a dual-stack IPv6 UDP socket.
	//
	// Using AF_INET6 with IPV6_V6ONLY=0 gives us a single socket that accepts
	// both IPv6 senders (natively) and IPv4 senders (as IPv4-mapped IPv6
	// addresses, e.g. ::ffff:192.168.1.5).  The Android sender auto-selects
	// the right address family based on the IP string the user entered, and
	// either will reach this socket without any extra configuration.
	// -----------------------------------------------------------------------
	socket_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (socket_ == INVALID_SOCKET) {
	#ifdef _WIN32
			blog(LOG_ERROR, "Socket creation failed. Error: %d", WSAGetLastError());
	#else
			blog(LOG_ERROR, "Socket creation failed.");
	#endif
			return false;
		}

	// Allow IPv4 clients to connect via IPv4-mapped addresses (::ffff:x.x.x.x)
	// Must be set before bind().
	int v6only = 0;
	#ifdef _WIN32
	if (setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY,
				(const char *)&v6only, sizeof(v6only)) == SOCKET_ERROR) {
		blog(LOG_WARNING, "Could not clear IPV6_V6ONLY — IPv4 clients may not connect");
	}
	#else
	if (setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY,
				&v6only, sizeof(v6only)) < 0) {
		blog(LOG_WARNING, "Could not clear IPV6_V6ONLY — IPv4 clients may not connect");
	}
	#endif

	// Large receive buffer
	int recv_buf = 16 * 1024 * 1024;
	#ifdef _WIN32
	setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
			(const char *)&recv_buf, sizeof(recv_buf));
	#else
	setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf));
	#endif

	// DSCP
	int dscp = 0xB8;
	#ifdef _WIN32
	setsockopt(socket_, IPPROTO_IPV6, IPV6_TCLASS,
			(const char *)&dscp, sizeof(dscp));
	#else
	setsockopt(socket_, IPPROTO_IPV6, IPV6_TCLASS, &dscp, sizeof(dscp));
	#endif

	// Reuse addr
	int reuse = 1;
	#ifdef _WIN32
	setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
			(const char *)&reuse, sizeof(reuse));
	#else
	setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	#endif

		// Non-blocking — receive_frame() returns immediately when socket is empty
	#ifdef _WIN32
		u_long mode = 1;
		ioctlsocket(socket_, FIONBIO, &mode);
	#else
		int flags = fcntl(socket_, F_GETFL, 0);
		fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
	#endif

	// Bind to in6addr_any — listens on all interfaces, both IPv4 and IPv6
	sockaddr_in6 addr = {};
	addr.sin6_family = AF_INET6;
	addr.sin6_addr   = in6addr_any;
	addr.sin6_port   = htons(port);

	if (bind(socket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))
	    == SOCKET_ERROR) {
	#ifdef _WIN32
		blog(LOG_ERROR, "Bind failed on port %d, error %d",
		     port, WSAGetLastError());
	#else
		blog(LOG_ERROR, "Bind failed on port %d", port);
	#endif
		closesocket(socket_);
		socket_ = INVALID_SOCKET;
		return false;
	}
	
	// Clear/initialize frame slots so stale sequence numbers don't collide
	// with a newly started stream (e.g. after the phone changes resolution
	// or restarts streaming). Using an impossible sequence value ensures the
	// first packet for any real sequence will always reset the slot state.
	for (auto &s : slots) {
		s.sequence = UINT32_MAX;
		s.packets_received = 0;
		s.total_expected = 0;
		s.max_chunk_size = 0;
		s.actual_size = 0;
		s.data.clear();
		s.packet_map.clear(); 
		s.is_complete = false;
	}

	blog(LOG_INFO, "UDP receiver started on port %d (dual-stack IPv4+IPv6)", port);
	return true;
}

void UdpReceiver::stop()
{
	if (socket_ != INVALID_SOCKET) {
		closesocket(socket_);
		socket_ = INVALID_SOCKET;
	}
}

bool UdpReceiver::wait_for_frame_ready(int timeout_ms)
{
	if (socket_ == INVALID_SOCKET)
		return false;

	// Use select() to wait for socket readability with timeout.
	// This eliminates busy polling and sleep — the thread blocks efficiently
	// until data arrives or the timeout expires.
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(socket_, &readfds);

	struct timeval tv;
	tv.tv_sec  = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	int select_result = select(
		static_cast<int>(socket_) + 1,
		&readfds,
		nullptr,
		nullptr,
		&tv);

	// select() returns > 0 if socket is readable, 0 on timeout, -1 on error
	return select_result > 0;
}

bool UdpReceiver::receive_frame(std::vector<uint8_t> &frame_data)
{
    if (socket_ == INVALID_SOCKET)
        return false;

    uint8_t buffer[1500];

    sockaddr_storage sender_addr{};
#ifdef _WIN32
    int sender_len = sizeof(sender_addr);
#else
    socklen_t sender_len = sizeof(sender_addr);
#endif

    int received = recvfrom(socket_, (char *)buffer, sizeof(buffer), 0,
                            reinterpret_cast<sockaddr *>(&sender_addr),
                            &sender_len);

    if (received <= 0)
        return false;

    if (received < static_cast<int>(sizeof(PacketHeader)))
        return false;

    PacketHeader *header = reinterpret_cast<PacketHeader *>(buffer);

    uint8_t *packet_data = buffer + sizeof(PacketHeader);
    size_t packet_data_size =
        static_cast<size_t>(received) - sizeof(PacketHeader);

    const uint32_t seq        = header->sequence_number;
    const uint32_t total_pkts = header->total_packets;
    const uint32_t pkt_idx    = header->packet_index;
    const uint32_t max_chunk  = header->data_size;

    if (total_pkts == 0 || total_pkts > 10000) return false;
    if (pkt_idx >= total_pkts)                 return false;
    if (max_chunk == 0 || max_chunk > 65000)   return false;

    int slot_idx = seq % SLOT_COUNT;
    FrameSlot &slot = slots[slot_idx];

    // New sequence entering this slot
    if (slot.sequence != seq) {

        // If old frame was incomplete, log it
        if (!slot.is_complete && slot.packets_received > 0) {
            blog(LOG_WARNING,
                 "Dropping incomplete frame seq=%u (%u/%u)",
                 slot.sequence,
                 slot.packets_received,
                 slot.total_expected);
        }

        slot.sequence         = seq;
        slot.packets_received = 0;
        slot.total_expected   = total_pkts;
        slot.max_chunk_size   = max_chunk;
        slot.is_complete      = false;
        slot.actual_size      = 0;

        size_t worst_case = static_cast<size_t>(max_chunk) * total_pkts;

        if (slot.data.size() < worst_case)
            slot.data.resize(worst_case);

        // Initialize packet tracking
        slot.packet_map.assign(total_pkts, 0);
    }

    // Extra safety: header inconsistency protection
    if (slot.total_expected != total_pkts) {
        blog(LOG_WARNING,
             "Mismatched total_pkts for seq=%u (old=%u new=%u)",
             seq,
             slot.total_expected,
             total_pkts);
        return false;
    }

    size_t offset = static_cast<size_t>(pkt_idx) * max_chunk;

    if (offset + packet_data_size > slot.data.size()) {
        blog(LOG_WARNING,
             "Packet overflows slot (seq=%u idx=%u) — dropping",
             seq, pkt_idx);
        return false;
    }

    // Only count packet once
    if (!slot.packet_map[pkt_idx]) {

        std::memcpy(slot.data.data() + offset,
                    packet_data,
                    packet_data_size);

        slot.packet_map[pkt_idx] = 1;
        slot.packets_received++;

        size_t end_byte = offset + packet_data_size;
        if (end_byte > slot.actual_size)
		{slot.actual_size = end_byte;}
    }

    // Frame complete
    if (slot.packets_received == slot.total_expected &&
        !slot.is_complete) {

        slot.is_complete = true;

        frame_data.assign(slot.data.begin(),
                          slot.data.begin() + slot.actual_size);

		blog(LOG_DEBUG, "Frame complete seq=%u size=%zu",
     	slot.sequence,
     	slot.actual_size);

        return true;
    }

    return false;
}