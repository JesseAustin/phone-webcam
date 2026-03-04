#pragma once

#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
#define closesocket    close
#endif

struct FrameSlot {
	uint32_t             sequence        = 0;
	uint32_t             packets_received = 0;
	uint32_t             total_expected  = 0;
	uint32_t             max_chunk_size  = 0;
	size_t               actual_size     = 0;
	std::vector<uint8_t> packet_map;
	std::vector<uint8_t> data;
	bool                 is_complete     = false;
};

class UdpReceiver {
public:
	UdpReceiver();
	~UdpReceiver();

	bool start(uint16_t port);
	void stop();
	
	// Event-driven: wait for socket readability with timeout (returns false on timeout).
	// Use this instead of polling/sleep to eliminate busy-waiting.
	bool wait_for_frame_ready(int timeout_ms);
	
	bool receive_frame(std::vector<uint8_t> &frame_data);

private:

	// Set numbe of slots here:
	static constexpr int SLOT_COUNT = 8;

	SOCKET socket_;
	bool   initialized_;

	FrameSlot slots[SLOT_COUNT];

	struct PacketHeader {
		uint32_t sequence_number;
		uint32_t total_packets;
		uint32_t packet_index;
		uint32_t data_size;
	};

	//uint32_t current_sequence_;
	//uint32_t packets_received_;
	//uint32_t total_packets_;
};
