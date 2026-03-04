#pragma once

#include <obs-module.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "udp-receiver.h"
#include "jpeg-decoder.h"
#include "mdns-discovery.h"

#define PLUGIN_VERSION "1.0.0"

struct phone_source {
	obs_source_t *source;

	// Components
	std::unique_ptr<UdpReceiver>   receiver;
	std::unique_ptr<JpegDecoder>   decoder;
	std::unique_ptr<MdnsDiscovery> discovery;

	// Threads
	std::thread       network_thread;
	std::thread       reconnect_thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> reconnect_pending{ false };

	// Settings
	uint16_t port{ 0 };

	// Frame buffer — written by network thread under frame_mutex,
	// then handed to obs_source_output_video
	std::mutex            frame_mutex;
	std::vector<uint8_t>  back_buffer;
	uint32_t              back_width{ 0 };
	uint32_t              back_height{ 0 };

	// Dimensions read by OBS render thread — atomic for lock-free access
	std::atomic<uint32_t> frame_width{ 1920 };
	std::atomic<uint32_t> frame_height{ 1080 };
	uint64_t              frame_timestamp{ 0 };
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