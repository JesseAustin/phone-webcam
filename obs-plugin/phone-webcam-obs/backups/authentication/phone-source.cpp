// phone-source.cpp  (password-aware diff — only changed sections shown with full context)
//
// Changes from original:
//   1. phone_source_get_defaults   — default password = ""
//   2. phone_source_get_properties — adds password text field (OBS_TEXT_PASSWORD)
//   3. phone_source_update         — reads new password, calls setPassword() + reconnects
//                                    (same live behavior as port field)
//   4. All HandshakeClient::sendHandshake() calls — pass ctx->password

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

const char *phone_source_get_name(void * /*unused*/)
{
	return "Phone Webcam";
}

// ---------------------------------------------------------------------------
// Reconnect thread — unchanged from original
// ---------------------------------------------------------------------------
static void launchReconnectThread(phone_source* ctx)
{
	if (!ctx || !ctx->valid) return;
	if (ctx->reconnect_pending.exchange(true)) return;

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
		}

		// Handshake retries — only if we have a known phone IP
		if (!ctx->mdns_mode && ctx->running && !ctx->lastIp.empty()) {
			uint16_t port = ctx->port.load();
			std::string pw;
			{
				std::lock_guard<std::mutex> lock(ctx->password_mutex);
				pw = ctx->password;
			}
			for (int i = 0; i < 5 && ctx->valid; i++) {
				if (HandshakeClient::sendHandshake(ctx->lastIp, port, pw)) {
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
// Network thread — unchanged from original
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
	int noFrameMs = 0;

	while (ctx->running && ctx->valid) {

		if (!ctx->receiver->wait_for_frame_ready(5)) {
			noFrameMs += 5;
			continue;
		}

		noFrameMs = 0;
		if (!ctx->valid) break;

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
				latest_bgra   = std::move(bgra_data);
				latest_width  = width;
				latest_height = height;
				have_latest   = true;
			}
		}

		if (!ctx->valid) break;

		if (have_latest) {
			ctx->frame_width  = latest_width;
			ctx->frame_height = latest_height;

			std::lock_guard<std::mutex> lock(ctx->frame_mutex);

			video_format fmt = ctx->decoder->outputFormat();
			size_t needed = 0;
			if (fmt == VIDEO_FORMAT_BGRA) {
				needed = (size_t)latest_width * latest_height * 4;
			} else if (fmt == VIDEO_FORMAT_NV12) {
				size_t y_size = (size_t)latest_width * latest_height;
				needed = y_size + y_size / 2;
			} else if (fmt == VIDEO_FORMAT_I420) {
				needed = (size_t)latest_width * latest_height * 3 / 2;
			} else {
				blog(LOG_ERROR, "Network thread: unsupported format: %d", (int)fmt);
				continue;
			}

			if (latest_bgra.size() < needed) {
				blog(LOG_ERROR, "Network thread: buffer too small — got %zu, expected %zu",
					latest_bgra.size(), needed);
				continue;
			}

			ctx->back_buffer.swap(latest_bgra);

			struct obs_source_frame frame = {};
			frame.format    = fmt;
			frame.width     = latest_width;
			frame.height    = latest_height;
			frame.timestamp = os_gettime_ns();

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

			obs_source_output_video(ctx->source, &frame);
		}
	}

	blog(LOG_INFO, "Network thread exiting%s", mdns_mode ? " (mDNS)" : "");
	if (ctx->valid)
		launchReconnectThread(ctx);
}

// ---------------------------------------------------------------------------
void *phone_source_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_INFO, "Creating phone webcam source");

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

	ctx->receiver  = std::make_unique<RtpReceiver>();
	ctx->decoder   = std::make_unique<H264Decoder>();
	ctx->discovery = std::make_unique<MdnsDiscovery>();

	// Load initial password from settings
	{
		std::lock_guard<std::mutex> lock(ctx->password_mutex);
		ctx->password = obs_data_get_string(settings, "password");
	}

	// Discovery callback
	ctx->discovery->start([ctx](const std::string& ip, uint16_t port) {
		if (!ctx || !ctx->valid) return;

		blog(LOG_INFO, "Auto-discovered phone at %s:%d", ip.c_str(), port);
		ctx->discovery->pause();

		std::lock_guard<std::mutex> lock(ctx->op_mutex);
		if (!ctx->valid) return;

		if (ctx->running && ctx->port.load() == port) {
			blog(LOG_INFO, "mDNS: re-discovered same service, requesting keyframe");
			return;
		}

		if (ctx->running) {
			ctx->running = false;
			ctx->receiver->stop();
			if (ctx->network_thread.joinable()) {
				ctx->network_thread.join();
				ctx->network_thread = std::thread();
			}
		}

		if (!ctx->valid) return;

		ctx->port = port;

		if (!ctx->receiver->start(port)) {
			blog(LOG_ERROR, "mDNS: Failed to start receiver on port %d", (int)port);
			ctx->discovery->resume();
			return;
		}

		ctx->running = true;
		ctx->network_thread = std::thread(runNetworkThread, ctx, true);

		{
			std::string ip_copy   = ip;
			uint16_t    port_copy = port;

			if (ctx->handshake_thread.joinable()) {
				ctx->handshake_thread.join();
				ctx->handshake_thread = std::thread();
			}

			ctx->handshake_thread = std::thread([ctx, ip_copy, port_copy]() {
				if (!ctx || !ctx->valid) return;

				// Read password safely
				std::string pw;
				{
					std::lock_guard<std::mutex> lock(ctx->password_mutex);
					pw = ctx->password;
				}

				if (!HandshakeClient::sendHandshake(ip_copy, port_copy, pw)) {
					blog(LOG_WARNING, "Handshake failed to %s:%d", ip_copy.c_str(), (int)port_copy);

					std::lock_guard<std::mutex> lock(ctx->op_mutex);
					if (!ctx->valid) return;

					ctx->running = false;
					ctx->receiver->stop();
					if (ctx->network_thread.joinable()) {
						ctx->network_thread.join();
						ctx->network_thread = std::thread();
					}
					if (ctx->valid && ctx->discovery)
						ctx->discovery->resume();
				}
			});
		}
	});

	ctx->handshakeServer = std::make_unique<HandshakeServer>();

	// Apply initial password to handshake server
	{
		std::lock_guard<std::mutex> lock(ctx->password_mutex);
		ctx->handshakeServer->setPassword(ctx->password);
	}

	// Handshake server callback (Android→OBS direction)
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

			ctx->port    = port;
			ctx->running = true;
			ctx->network_thread = std::thread(runNetworkThread, ctx, true);

			// Read password for outgoing reply handshake
			std::string pw;
			{
				std::lock_guard<std::mutex> lock2(ctx->password_mutex);
				pw = ctx->password;
			}
			HandshakeClient::sendHandshake(phoneIp, port, pw);
			ctx->lastIp = phoneIp;
		}).detach();
	});

	return ctx;
}

void phone_source_destroy(void *data)
{
	blog(LOG_INFO, "Destroying phone webcam source");
	auto *ctx = static_cast<phone_source *>(data);

	ctx->valid   = false;
	ctx->running = false;

	std::unique_lock<std::mutex> lock(ctx->op_mutex);

	ctx->discovery->stop();
	ctx->handshakeServer->stop();
	ctx->receiver->stop();

	if (ctx->network_thread.joinable()) {
		ctx->network_thread.join();
		ctx->network_thread = std::thread();
	}
	if (ctx->reconnect_thread.joinable()) {
		ctx->reconnect_thread.join();
		ctx->reconnect_thread = std::thread();
	}
	if (ctx->handshake_thread.joinable()) {
		ctx->handshake_thread.join();
		ctx->handshake_thread = std::thread();
	}

	lock.unlock();
	ctx->back_buffer.clear();
	delete ctx;
}

// ---------------------------------------------------------------------------
// phone_source_update — fires on every property change in the OBS UI.
//
// Password field behavior mirrors port field:
//   - Reads new value
//   - Updates ctx->password (under lock)
//   - Updates handshakeServer password immediately (thread-safe)
//   - If streaming, stops stream + fires launchReconnectThread
//     → reconnect re-handshakes with the new password automatically
//
// The user never needs to press a button.
// ---------------------------------------------------------------------------
void phone_source_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<phone_source *>(data);
    if (!ctx || !ctx->valid) return;

    const uint16_t new_port = static_cast<uint16_t>(obs_data_get_int(settings, "port"));
    const char* pw_cstr     = obs_data_get_string(settings, "password");
    std::string new_password = pw_cstr ? pw_cstr : "";

    blog(LOG_INFO, "Phone webcam source updated — port: %d, auth: %s",
         (int)new_port, new_password.empty() ? "none" : "password set");

    ctx->port    = new_port;
    ctx->mdns_mode = false;

    // Update password atomically — handshakeServer picks it up per-connection
    {
        std::lock_guard<std::mutex> lock(ctx->password_mutex);
        ctx->password = new_password;
    }
    ctx->handshakeServer->setPassword(new_password);

    if (ctx->running) {
        ctx->running = false;
        ctx->receiver->stop();
    }

    launchReconnectThread(ctx);
}

void phone_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", 9000);
	obs_data_set_default_string(settings, "password", "");
}

obs_properties_t *phone_source_get_properties(void * /*data*/)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "port", "RTP Port", 1024, 65535, 1);

	// OBS_TEXT_PASSWORD renders as a masked field with a show/hide toggle
	obs_properties_add_text(props, "password", "Stream Password (optional)",
	                        OBS_TEXT_PASSWORD);

	obs_properties_add_text(props, "info",
		"Leave password blank for no security.\n"
		"If set, the Android app must use the same password.\n"
		"Changing the password will reconnect the stream automatically.",
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