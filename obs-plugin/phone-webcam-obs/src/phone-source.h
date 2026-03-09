#pragma once

#include <obs-module.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include "srtp-session.h"
#include "aac-decoder.h"
#include "rtp-receiver.h"
#include "frame-decoder.h"
#include "mdns-discovery.h"
#include "handshake-server.h"

#define PLUGIN_VERSION "1.0.0"

struct phone_source {
	obs_source_t *source;

	// Components
	std::unique_ptr<RtpReceiver>     receiver;
	std::unique_ptr<IFrameDecoder>   decoder;
	std::unique_ptr<MdnsDiscovery>   discovery;
	std::unique_ptr<HandshakeServer> handshakeServer;

	// Threads
	std::thread       network_thread;
	std::thread       reconnect_thread;
	std::thread       handshake_thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> reconnect_pending{ false };
	std::atomic<bool> valid{ true };

	// Settings
	std::atomic<uint16_t> port{ 0 };
	std::string           lastIp;   // IP from last successful Android handshake
	std::string           manualIp; // IP typed manually in OBS properties (fallback)

	// -----------------------------------------------------------------------
	// Stream password — protected by its own mutex.
	// Read by handshake threads, written by phone_source_update on the OBS
	// UI thread. Never stored as plaintext in obs_data after initial load;
	// obs_data is the source of truth only at create/update time.
	// -----------------------------------------------------------------------
	std::mutex  password_mutex;
	std::string password;

	// Frame buffer
	std::mutex            frame_mutex;
	std::vector<uint8_t>  back_buffer;
	uint32_t              back_width{ 0 };
	uint32_t              back_height{ 0 };

	// Dimensions — atomic for lock-free access by OBS render thread
	std::atomic<uint32_t> frame_width{ 1920 };
	std::atomic<uint32_t> frame_height{ 1080 };
	uint64_t              frame_timestamp{ 0 };

	// Audio
	std::unique_ptr<AacDecoder> audio_decoder;
	std::thread                 audio_thread;
	std::atomic<bool>           audio_running{false};
	SOCKET                      audio_socket{INVALID_SOCKET};
	uint16_t                    audio_port{0};
	
	// SRTP Sessions for encryption
	SrtpSession                 video_srtp;  
	SrtpSession 				audio_srtp;

	bool mdns_mode = true;
	std::mutex op_mutex;

	std::mutex  srtp_salt_mutex;
	std::string srtp_salt_hex;
};

extern struct obs_source_info phone_source_info;

const char       *phone_source_get_name(void *unused);
void             *phone_source_create(obs_data_t *settings, obs_source_t *source);
void              phone_source_destroy(void *data);
void              phone_source_update(void *data, obs_data_t *settings);
void              phone_source_get_defaults(obs_data_t *settings);
obs_properties_t *phone_source_get_properties(void *data);
uint32_t          phone_source_get_width(void *data);
uint32_t          phone_source_get_height(void *data);