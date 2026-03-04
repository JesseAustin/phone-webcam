#include "phone-source.h"
#include "handshake-client.h"
#include <obs-module.h>
#include <util/platform.h>
#include <thread>
#include "h264-decoder.h"

struct obs_source_info phone_source_info = {
	.id             = "phone_webcam_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = phone_source_get_name,
	.create         = phone_source_create,
	.destroy        = phone_source_destroy,
	.get_width      = phone_source_get_width,
	.get_height     = phone_source_get_height,
	.get_defaults   = phone_source_get_defaults,
	.get_properties = phone_source_get_properties,
	.update         = phone_source_update,
};

// Forward declarations
static void runNetworkThread(phone_source* ctx, bool mdns_mode);
static void launchReconnectThread(phone_source* ctx);

// Returns the display name shown in OBS's "Add Source" picker
const char *phone_source_get_name(void * /*unused*/)
{
	return "Phone Webcam";
}

// ---------------------------------------------------------------------------
// Reconnect thread — owns the join/reset/resume sequence after a disconnect.
// The network thread just sets running=false and exits; this does the rest.
// ---------------------------------------------------------------------------
static void launchReconnectThread(phone_source* ctx)
{
	if (!ctx || !ctx->valid) return;  // Safety check
	
	// exchange(true) atomically sets the flag and returns the OLD value.
    // If it was already true, a reconnect is in progress — bail out.
	if (ctx->reconnect_pending.exchange(true)) return;

	// If a previous reconnect thread is still joinable, wait for it first
	if (ctx->reconnect_thread.joinable()) {
		blog(LOG_DEBUG, "phone-source: joining previous reconnect_thread");
		ctx->reconnect_thread.join();
		ctx->reconnect_thread = std::thread();
	}

	ctx->reconnect_thread = std::thread([ctx]() {
		if (!ctx || !ctx->valid) { ctx->reconnect_pending = false; return; }

		obs_source_set_async_unbuffered(ctx->source, true);

		{
			std::lock_guard<std::mutex> lock(ctx->op_mutex);

			if (!ctx->valid) { ctx->reconnect_pending = false; return; }

			if (ctx->network_thread.joinable()) {
				ctx->network_thread.join();
				ctx->network_thread = std::thread();
			}

			ctx->receiver->stop();
			ctx->running = false;

			if (ctx->mdns_mode) {
				blog(LOG_INFO, "Reconnect: resuming mDNS discovery");
				ctx->discovery->resetDiscoveryState();
				ctx->discovery->resume();
			} else {
				uint16_t port = ctx->port.load();
				if (ctx->receiver->start(port)) {
					ctx->running = true;
					ctx->network_thread = std::thread(runNetworkThread, ctx, false);
				} else {
					blog(LOG_ERROR, "Reconnect: failed to restart receiver on port %d", port);
				}
			}
		} // lock released here

		// Handshake retries are blocking network calls — must be outside the lock
		if (!ctx->mdns_mode && ctx->running) {
			uint16_t port = ctx->port.load();
			for (int i = 0; i < 5 && ctx->valid; i++) {
				if (HandshakeClient::sendHandshake(ctx->lastIp, port)) {
					blog(LOG_INFO, "Reconnect: handshake success");
					break;
				}
				blog(LOG_WARNING, "Reconnect: handshake attempt %d failed", i + 1);
			}
		}

		ctx->reconnect_pending = false;
	});
}


static bool isH264(const std::vector<uint8_t>& data)
{
    if (data.size() < 4) return false;
    return (data[0] == 0x00 && data[1] == 0x00 &&
            data[2] == 0x00 && data[3] == 0x01) ||
           (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01);
}

// ---------------------------------------------------------------------------
// Shared network thread body — used by both mDNS and manual-port paths.
// ---------------------------------------------------------------------------
static void runNetworkThread(phone_source* ctx, bool mdns_mode)
{
	if (!ctx || !ctx->valid) {
		blog(LOG_WARNING, "Network thread started with invalid context");
		return;
	}

	std::vector<uint8_t> encoded_data;
	std::vector<uint8_t> bgra_data;
	std::vector<uint8_t> latest_bgra;
	uint32_t latest_width  = 0;
	uint32_t latest_height = 0;

	bool decoder_needs_flush = true;

	// tracks how long we've gone without a frame (in milliseconds)
	int noFrameMs = 0;

	while (ctx->running && ctx->valid) {
		
		if (!ctx->receiver->wait_for_frame_ready(5)) {
			noFrameMs += 5;
			// Don't timeout — just keep waiting. The phone may not be streaming yet,
        	// or may have briefly disconnected. We'll know it's truly gone if
        	// onDisconnected fires or the user stops the source.
			continue;
		}

		noFrameMs = 0;

		if (!ctx->valid) break;  // Safety check

		// Drain: pull all buffered frames, keep only the latest
		bool have_undecoded_frame = false;
		have_undecoded_frame = ctx->receiver->receive_frame(encoded_data);

		bool have_latest = false;
		if (have_undecoded_frame) {
		blog(LOG_INFO, "phone-source: detected H.264 frame");

		if (decoder_needs_flush && encoded_data.size() >= 5) {
			uint8_t nal_byte = encoded_data[4];
			int nal_type = nal_byte & 0x1F;
			if (nal_type == 7) {
				blog(LOG_INFO, "phone-source: flushing decoder for reconnect");
				ctx->decoder->flush();
				decoder_needs_flush = false;
			}
		}

		uint32_t width = 0, height = 0;
		if (ctx->decoder->decode(encoded_data, bgra_data, width, height)
			&& width > 0 && height > 0) {
				blog(LOG_DEBUG, "Network thread: decoded frame %ux%u", width, height);
				latest_bgra   = std::move(bgra_data);
				latest_width  = width;
				latest_height = height;
				have_latest   = true;
		}
	}

		if (!ctx->valid) break;  // Safety check

		if (have_latest) {

			ctx->frame_width  = latest_width;
    		ctx->frame_height = latest_height;

			std::lock_guard<std::mutex> lock(ctx->frame_mutex);


			blog(LOG_INFO, "decoder type: %s", typeid(*ctx->decoder).name());
			video_format fmt = ctx->decoder->outputFormat();
			blog(LOG_INFO, "decoder outputFormat returned: %d", (int)fmt);
			
			// Get the actual output format from the active decoder
			fmt = ctx->decoder->outputFormat();
			
			// Calculate buffer size based on actual format
			size_t needed = 0;
			
			if (fmt == VIDEO_FORMAT_BGRA) {
				needed = (size_t)latest_width * latest_height * 4;
			} else if (fmt == VIDEO_FORMAT_NV12) {
				size_t y_size = (size_t)latest_width * latest_height;
    			needed = y_size + y_size / 2;	
			} 
			else if (fmt == VIDEO_FORMAT_I420) {
				needed = (size_t)latest_width * latest_height * 3 / 2;
			}
			else {
				blog(LOG_ERROR, "Network thread: unsupported decoder format: %d", (int)fmt);
				continue;
			}

			// Verify the decoder gave us exactly the buffer size we expect
			// before handing a pointer into it to OBS.
			if (latest_bgra.size() < needed) {
				blog(LOG_ERROR, "Network thread: buffer too small — got %zu, expected %zu",
					latest_bgra.size(), needed);
				continue;
			}
			
			// Faster than copying if the decoder already wrote directly into back_buffer
			ctx->back_buffer.swap(latest_bgra);

			struct obs_source_frame frame = {};
			frame.format          = fmt;
			frame.width           = latest_width;
			frame.height          = latest_height;
			frame.timestamp       = os_gettime_ns();

			size_t y_size = 0;
			size_t u_size = 0;
			
			if (fmt == VIDEO_FORMAT_BGRA) {
				frame.data[0]     = ctx->back_buffer.data();
				frame.linesize[0] = latest_width * 4;
			} else if (fmt == VIDEO_FORMAT_I420) {
				size_t y_size = (size_t)latest_width * latest_height;
				size_t u_size = y_size / 4;
				frame.data[0]     = ctx->back_buffer.data();
				frame.linesize[0] = latest_width;
				frame.data[1]     = ctx->back_buffer.data() + y_size;
				frame.linesize[1] = latest_width / 2;
				frame.data[2]     = ctx->back_buffer.data() + y_size + u_size;
				frame.linesize[2] = latest_width / 2;
			}

			/*
			else if (fmt == VIDEO_FORMAT_NV12) {
				size_t y_size = (size_t)latest_width * latest_height;

    			frame.data[0]     = ctx->back_buffer.data();
    			frame.linesize[0] = latest_width;
    			frame.data[1]     = ctx->back_buffer.data() + y_size;
    			frame.linesize[1] = latest_width; 
			}*/

			// Check first few bytes of Y plane are non-zero
			uint8_t* y = ctx->back_buffer.data();
			blog(LOG_INFO, "phone-source: Y plane sample bytes: %02X %02X %02X %02X %02X",
				y[0], y[1], y[2], y[3], y[4]);

			blog(LOG_INFO, "I420 frame: data[0]=%p data[1]=%p data[2]=%p ls=%d/%d/%d",
    		frame.data[0], frame.data[1], frame.data[2],
    		frame.linesize[0], frame.linesize[1], frame.linesize[2]);

			blog(LOG_INFO, "frame: width=%u height=%u linesize=%u buf_size=%zu",
    		frame.width, frame.height, frame.linesize[0], ctx->back_buffer.size());
			obs_source_output_video(ctx->source, &frame);
			blog(LOG_INFO, "phone-source: output video frame %ux%u fmt=%d", latest_width, latest_height, (int)fmt);
		}
	}

	blog(LOG_INFO, "Network thread exiting%s", mdns_mode ? " (mDNS)" : "");

	// Auto-reconnect is permanent:
	if (ctx->valid)
    	launchReconnectThread(ctx);
}

// ---------------------------------------------------------------------------

void *phone_source_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_INFO, "Creating phone webcam source");
	blog(LOG_ERROR, 
     "ENUM VALUES: I420=%d NV12=%d BGRA=%d", 
     VIDEO_FORMAT_I420, 
     VIDEO_FORMAT_NV12,
	 VIDEO_FORMAT_BGRA);

	auto *ctx            = new phone_source();
	ctx->source          = source;
	obs_source_set_async_unbuffered(ctx->source, true);
	ctx->running         = false;
	ctx->valid           = true;
	ctx->back_buffer.clear();
	ctx->back_width      = 0;
	ctx->back_height     = 0;
	ctx->frame_width     = 1920;
	ctx->frame_height    = 1080;
	ctx->frame_timestamp = 0;

	// Define receiver, decoder, and discovery components.
	ctx->receiver = std::make_unique<RtpReceiver>();
	ctx->decoder  = std::make_unique<H264Decoder>();
	ctx->discovery = std::make_unique<MdnsDiscovery>();

	// Discovery callback — runs on the discovery thread when a phone is found
	ctx->discovery->start([ctx](const std::string& ip, uint16_t port) {
		// Safety check: object may have been destroyed while callback was queued
		if (!ctx || !ctx->valid) {
			blog(LOG_WARNING, "phone-source: discovery callback fired after destruction");
			return;
		}

		blog(LOG_INFO, "Auto-discovered phone at %s:%d", ip.c_str(), port);

		ctx->discovery->pause();

		// Lock to prevent concurrent reconnects or network thread 
		// operations while we handle this new discovery:
		std::lock_guard<std::mutex> lock(ctx->op_mutex);


		if (!ctx->valid) return;  // Check again after pause

		if (ctx->running && ctx->port.load() == port) {
			blog(LOG_INFO, "mDNS: re-discovered same service, requesting keyframe");
			// Optionally: signal decoder to request IDR
			return;
		}

		if (ctx->running) {
			ctx->running = false;
			ctx->receiver->stop();
			if (ctx->network_thread.joinable()) {
				blog(LOG_DEBUG, "phone-source: joining network_thread in discovery callback");
				ctx->network_thread.join();
				ctx->network_thread = std::thread();
			}
		}

		if (!ctx->valid) return;  // Check before continuing

		ctx->port = port;

		if (!ctx->receiver->start(port)) {
			blog(LOG_ERROR, "mDNS: Failed to start receiver on port %d", (int)port);
			ctx->discovery->resume();
			return;
		}

		ctx->running = true;
		ctx->network_thread = std::thread(runNetworkThread, ctx, true);

		// Perform handshake asynchronously to avoid blocking the discovery
		// callback thread. If handshake fails, stop the receiver and resume
		// discovery so another response can be tried.
		{
			std::string ip_copy = ip;
			uint16_t port_copy = port;

			// Join any previous handshake thread before starting a new one
			if (ctx->handshake_thread.joinable()) {
				ctx->handshake_thread.join();
				ctx->handshake_thread = std::thread();
			}

			ctx->handshake_thread = std::thread([ctx, ip_copy, port_copy]() {
				if (!ctx || !ctx->valid) return;  // Check at thread start
				
				if (!HandshakeClient::sendHandshake(ip_copy, port_copy)) {
					blog(LOG_WARNING, "Handshake failed to %s:%d", ip_copy.c_str(), (int)port_copy);
					
					// Inner lock to safely stop receiver and resume discovery 
					// without racing with other operations				
					std::lock_guard<std::mutex> lock(ctx->op_mutex); 

					if (!ctx->valid) return;  // Check before accessing
					
					ctx->running = false;
					ctx->receiver->stop();
					if (ctx->network_thread.joinable()) {
						blog(LOG_DEBUG, "phone-source: joining network_thread after handshake failure (async)");
						ctx->network_thread.join();
						ctx->network_thread = std::thread();
					}
					
					if (ctx->valid && ctx->discovery)
    					ctx->discovery->resume();
					return;
				}
				
			});
			//ctx->handshake_thread.detach();
		}

	});

	ctx->handshakeServer = std::make_unique<HandshakeServer>();

	// Handshake callback — runs on the handshake server thread when Android initiates a handshake
	ctx->handshakeServer->start([ctx](const std::string& phoneIp) {
		if (!ctx || !ctx->valid) return;

		blog(LOG_INFO, "HandshakeServer: Android requested handshake from %s", phoneIp.c_str());

		uint16_t port = ctx->port.load();
		if (port == 0) port = 9000;

		if (ctx->running) {
			ctx->running = false;
			ctx->receiver->stop();
		}

		std::thread([ctx, phoneIp, port]() {
			if (!ctx || !ctx->valid) return;

			std::lock_guard<std::mutex> lock(ctx->op_mutex);
			if (!ctx->valid) return;

			if (ctx->network_thread.joinable()) {
				ctx->network_thread.join();
				ctx->network_thread = std::thread();
			}

			if (!ctx->valid) return;

			if (!ctx->receiver->start(port)) {
				blog(LOG_ERROR, "HandshakeServer: failed to start receiver");
				return;
			}

			ctx->port = port;
			ctx->running = true;
			ctx->network_thread = std::thread(runNetworkThread, ctx, true);
			HandshakeClient::sendHandshake(phoneIp, port);
			ctx->lastIp = phoneIp;
		}).detach();
	});


	return ctx;
}

void phone_source_destroy(void *data)
{
	blog(LOG_INFO, "Destroying phone webcam source");
	auto *ctx = static_cast<phone_source *>(data);

	// CRITICAL: Mark as invalid FIRST to prevent use-after-free in any pending callbacks
	ctx->valid = false;
	ctx->running = false;

	// Acquire BEFORE stopping anything — forces any in-progress op_mutex
    // section in other threads to finish before we start joining
    std::unique_lock<std::mutex> lock(ctx->op_mutex);

	ctx->discovery->stop();
	ctx->handshakeServer->stop();
	ctx->receiver->stop();

	if (ctx->network_thread.joinable()) {
		blog(LOG_INFO, "phone-source: joining network_thread during destroy");
		ctx->network_thread.join();
		ctx->network_thread = std::thread();
	}
	if (ctx->reconnect_thread.joinable()) {
		blog(LOG_INFO, "phone-source: joining reconnect_thread during destroy");
		ctx->reconnect_thread.join();
		ctx->reconnect_thread = std::thread();
	}
	if (ctx->handshake_thread.joinable()) {
		blog(LOG_INFO, "phone-source: joining handshake_thread during destroy");
		ctx->handshake_thread.join();
		ctx->handshake_thread = std::thread();
	}

	lock.unlock(); // explicit unlock before delete
	ctx->back_buffer.clear();
	delete ctx;
}

void phone_source_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<phone_source *>(data);
    if (!ctx || !ctx->valid) return;

    const uint16_t new_port = static_cast<uint16_t>(obs_data_get_int(settings, "port"));
    blog(LOG_INFO, "Phone webcam source updated — port: %d", (int)new_port);

    ctx->port = new_port;
    ctx->mdns_mode = false; // Port was manually set

    if (ctx->running) {
        ctx->running = false;
        ctx->receiver->stop();
    }

    launchReconnectThread(ctx); // handles everything else
}

void phone_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", 9000);
}

obs_properties_t *phone_source_get_properties(void * /*data*/)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "port", "RTP Port", 1024, 65535, 1);
	obs_properties_add_text(props, "info",
		"Connect your phone to the same Wi-Fi network and open the\n"
		"Phone Webcam app. Enter this port number in the app settings.",
		OBS_TEXT_INFO);
	return props;
}

uint32_t phone_source_get_width(void *data)
{
	return static_cast<phone_source *>(data)->frame_width;
}

uint32_t phone_source_get_height(void *data)
{
	return static_cast<phone_source *>(data)->frame_height;
}