#include "phone-source.h"
#include "handshake-client.h"
#include <obs-module.h>
#include <util/platform.h>
#include <thread>
#include <functional>

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

// Returns the display name shown in OBS's "Add Source" picker
const char *phone_source_get_name(void * /*unused*/)
{
	return "OBS Webcam";
}

// ---------------------------------------------------------------------------
// Reconnect thread — owns the join/reset/resume sequence after a disconnect.
// The network thread just sets running=false and exits; this does the rest.
// ---------------------------------------------------------------------------
static void launchReconnectThread(phone_source* ctx)
{
	// exchange(true) atomically sets the flag and returns the OLD value.
    // If it was already true, a reconnect is in progress — bail out.
	if (ctx->reconnect_pending.exchange(true)) return;

	// If a previous reconnect thread is still joinable, wait for it first
	if (ctx->reconnect_thread.joinable()) {
		// Diagnostic before join
		size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
		size_t tid_target = std::hash<std::thread::id>{}(ctx->reconnect_thread.get_id());
		blog(LOG_DEBUG, "phone-source: reconnect_thread join check this=%zu target=%zu joinable=%d",
			 tid_this, tid_target, (int)ctx->reconnect_thread.joinable());

		// Avoid joining the current thread (would deadlock/crash)
		if (ctx->reconnect_thread.get_id() != std::this_thread::get_id()) {
			blog(LOG_DEBUG, "phone-source: joining previous reconnect_thread");
			ctx->reconnect_thread.join();
		}
		ctx->reconnect_thread = std::thread();
	}

	ctx->reconnect_thread = std::thread([ctx]() {

		// Wait for the network thread to fully exit before touching anything.
		// But do NOT attempt to join if this reconnect thread is running on the
		// same thread as `network_thread` (joining self would crash).
		if (ctx->network_thread.joinable()) {
			size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
			size_t tid_target = std::hash<std::thread::id>{}(ctx->network_thread.get_id());
			blog(LOG_DEBUG, "phone-source: network_thread join check this=%zu target=%zu joinable=%d",
				 tid_this, tid_target, (int)ctx->network_thread.joinable());

			if (ctx->network_thread.get_id() != std::this_thread::get_id()) {
				blog(LOG_DEBUG, "phone-source: joining network_thread before reset");
				ctx->network_thread.join();
			}
			ctx->network_thread = std::thread();
		}

		blog(LOG_INFO, "Reconnect thread: resetting discovery state");

		// Unstick OBS's internal async video buffer after reconnect
		obs_source_set_async_unbuffered(ctx->source, true);

		// Clear the "already saw this service" flag so the next
        // mDNS SRV response fires the discovery callback again
		ctx->discovery->resetDiscoveryState();


		ctx->reconnect_pending = false;

		// Start sending PTR queries and listening for responses again
		ctx->discovery->resume();
	});
}

// ---------------------------------------------------------------------------
// Shared network thread body — used by both mDNS and manual-port paths.
// ---------------------------------------------------------------------------
static void runNetworkThread(phone_source* ctx, bool mdns_mode)
{
	std::vector<uint8_t> jpeg_data;
	std::vector<uint8_t> bgra_data;
	std::vector<uint8_t> latest_bgra;
	uint32_t latest_width  = 0;
	uint32_t latest_height = 0;

	// tracks how long we've gone without a frame (in milliseconds)
	int noFrameMs = 0;

	while (ctx->running) {
		if (!ctx->receiver->wait_for_frame_ready(5)) {
			noFrameMs += 5;
			if (noFrameMs >= 5000) {
				blog(LOG_INFO, "Network thread: no frames for 5s, assuming phone disconnected");
				ctx->running = false;
				break;
			}
			continue;
		}
		noFrameMs = 0;

		// Drain: pull all buffered frames, keep only the latest
		bool have_undecoded_frame = false;
		while (ctx->receiver->receive_frame(jpeg_data))
			have_undecoded_frame = true;

		bool have_latest = false;
		if (have_undecoded_frame) {
			uint32_t width = 0, height = 0;
			if (ctx->decoder->decode(jpeg_data, bgra_data, width, height)
				&& width > 0 && height > 0) {
					blog(LOG_DEBUG, "Network thread: decoded frame %ux%u", width, height);
					latest_bgra   = std::move(bgra_data);
					latest_width  = width;
					latest_height = height;
					have_latest   = true;
			}
		}

		if (have_latest) {
			blog(LOG_DEBUG, "Network thread: outputting frame %ux%u to OBS", latest_width, latest_height);
			{
				std::lock_guard<std::mutex> lock(ctx->frame_mutex);
				size_t needed = (size_t)latest_width * latest_height * 4;
				if (ctx->back_buffer.size() != needed){
					ctx->back_buffer.resize(needed);
				}
				ctx->back_buffer.swap(latest_bgra);
				ctx->back_width  = latest_width;
				ctx->back_height = latest_height;
			}
			ctx->frame_width  = latest_width;
			ctx->frame_height = latest_height;

			struct obs_source_frame frame = {};
			frame.data[0]     = ctx->back_buffer.data();
			frame.linesize[0] = latest_width * 4;
			frame.width       = latest_width;
			frame.height      = latest_height;
			frame.format      = VIDEO_FORMAT_BGRA;
			frame.timestamp   = os_gettime_ns();
			obs_source_output_video(ctx->source, &frame);
		}
	}

	blog(LOG_INFO, "Network thread exiting%s", mdns_mode ? " (mDNS)" : "");

	// Only mDNS mode should trigger auto-reconnect via discovery
	if (mdns_mode)
		launchReconnectThread(ctx);
}

// ---------------------------------------------------------------------------

void *phone_source_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_INFO, "Creating phone webcam source");

	auto *ctx            = new phone_source();
	ctx->source          = source;
	ctx->running         = false;
	ctx->back_buffer.clear();
	ctx->back_width      = 0;
	ctx->back_height     = 0;
	ctx->frame_width     = 1920;
	ctx->frame_height    = 1080;
	ctx->frame_timestamp = 0;

	ctx->receiver = std::make_unique<UdpReceiver>();
	ctx->decoder  = std::make_unique<JpegDecoder>();

	ctx->discovery = std::make_unique<MdnsDiscovery>();
	ctx->discovery->start([ctx](const std::string& ip, uint16_t port) {
		blog(LOG_INFO, "Auto-discovered phone at %s:%d", ip.c_str(), port);

		ctx->discovery->pause();


		if (ctx->running && ctx->port == port) {
			// Already connected on this port, no need to re-handshake
			blog(LOG_INFO, "mDNS: already connected on port %d, skipping handshake", port);
			return;
		}

		if (ctx->running) {
			ctx->running = false;
			ctx->receiver->stop();
			if (ctx->network_thread.joinable()) {
				size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
				size_t tid_target = std::hash<std::thread::id>{}(ctx->network_thread.get_id());
				blog(LOG_DEBUG, "phone-source: discovery callback network_thread join check this=%zu target=%zu joinable=%d",
					 tid_this, tid_target, (int)ctx->network_thread.joinable());

				if (ctx->network_thread.get_id() != std::this_thread::get_id()) {
					blog(LOG_DEBUG, "phone-source: joining network_thread in discovery callback");
					ctx->network_thread.join();
				}
				ctx->network_thread = std::thread();
			}
		}

		ctx->port = port;

		if (!ctx->receiver->start(ctx->port)) {
			blog(LOG_ERROR, "mDNS: Failed to start receiver on port %d", port);
			ctx->discovery->resume();
			return;
		}

		ctx->running = true;
		ctx->network_thread = std::thread(runNetworkThread, ctx, true);

		// Send Handshake synchronously so we wait for the phone to accept
		// the redirect while the receiver + network thread are already running.
		// This avoids a race where the no-frame timeout fires before the
		// phone actually starts streaming.
		if (!HandshakeClient::sendHandshake(ip, port)) {
			blog(LOG_WARNING, "Handshake failed to %s:%d", ip.c_str(), port);

			// Stop network activity and let discovery resume so we can retry.
			ctx->running = false;
			ctx->receiver->stop();
			if (ctx->network_thread.joinable()) {
				size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
				size_t tid_target = std::hash<std::thread::id>{}(ctx->network_thread.get_id());
				blog(LOG_DEBUG, "phone-source: handshake-failure network_thread join check this=%zu target=%zu joinable=%d",
					 tid_this, tid_target, (int)ctx->network_thread.joinable());

				if (ctx->network_thread.get_id() != std::this_thread::get_id()) {
					blog(LOG_DEBUG, "phone-source: joining network_thread after handshake failure");
					ctx->network_thread.join();
				}
				ctx->network_thread = std::thread();
			}

			ctx->discovery->resume();
			return;
		}

	});

	return ctx;
}

void phone_source_destroy(void *data)
{
	blog(LOG_INFO, "Destroying phone webcam source");
	auto *ctx = static_cast<phone_source *>(data);

	ctx->running = false;
	ctx->discovery->stop();
	ctx->receiver->stop();

	if (ctx->network_thread.joinable()) {
		size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
		size_t tid_target = std::hash<std::thread::id>{}(ctx->network_thread.get_id());
		blog(LOG_INFO, "phone-source: destroy network_thread join check this=%zu target=%zu joinable=%d",
			 tid_this, tid_target, (int)ctx->network_thread.joinable());

		if (ctx->network_thread.get_id() != std::this_thread::get_id()) {
			blog(LOG_INFO, "phone-source: joining network_thread during destroy");
			ctx->network_thread.join();
		}
		ctx->network_thread = std::thread();
	}
	if (ctx->reconnect_thread.joinable()) {
		size_t tid_this2 = std::hash<std::thread::id>{}(std::this_thread::get_id());
		size_t tid_target2 = std::hash<std::thread::id>{}(ctx->reconnect_thread.get_id());
		blog(LOG_INFO, "phone-source: destroy reconnect_thread join check this=%zu target=%zu joinable=%d",
			 tid_this2, tid_target2, (int)ctx->reconnect_thread.joinable());

		if (ctx->reconnect_thread.get_id() != std::this_thread::get_id()) {
			blog(LOG_INFO, "phone-source: joining reconnect_thread during destroy");
			ctx->reconnect_thread.join();
		}
		ctx->reconnect_thread = std::thread();
	}

	ctx->back_buffer.clear();
	delete ctx;
}

void phone_source_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<phone_source *>(data);

	const uint16_t new_port =
		static_cast<uint16_t>(obs_data_get_int(settings, "port"));

	blog(LOG_INFO, "Phone webcam source updated — port: %d", new_port);

	if (ctx->running) {
		ctx->running = false;
		ctx->receiver->stop();
		if (ctx->network_thread.joinable()) {
			size_t tid_this = std::hash<std::thread::id>{}(std::this_thread::get_id());
			size_t tid_target = std::hash<std::thread::id>{}(ctx->network_thread.get_id());
			blog(LOG_DEBUG, "phone-source: update network_thread join check this=%zu target=%zu joinable=%d",
				 tid_this, tid_target, (int)ctx->network_thread.joinable());

			if (ctx->network_thread.get_id() != std::this_thread::get_id()) {
				blog(LOG_DEBUG, "phone-source: joining network_thread during update");
				ctx->network_thread.join();
			}
			ctx->network_thread = std::thread();
		}
	}

	ctx->port = new_port;

	if (!ctx->receiver->start(ctx->port)) {
		blog(LOG_ERROR, "Failed to start UDP receiver on port %d", ctx->port);
		return;
	}

	ctx->running = true;
	ctx->network_thread = std::thread(runNetworkThread, ctx, false);
}

void phone_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", 9000);
}

obs_properties_t *phone_source_get_properties(void * /*data*/)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "port", "UDP Port", 1024, 65535, 1);
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