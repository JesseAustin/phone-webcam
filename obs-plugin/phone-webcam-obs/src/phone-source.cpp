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
	.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = phone_source_get_name,
	.create         = phone_source_create,
	.destroy        = phone_source_destroy,
	.get_width      = phone_source_get_width,
	.get_height     = phone_source_get_height,
	.get_defaults   = phone_source_get_defaults,
	.get_properties = phone_source_get_properties,
	.update         = phone_source_update,

	// add camera icon:
	.icon_type  	= OBS_ICON_TYPE_CAMERA,
};

// Forward declarations
static void runNetworkThread(phone_source* ctx, bool mdns_mode);
static void launchReconnectThread(phone_source* ctx);
static void startAudioThread(phone_source* ctx, uint16_t video_port);
static void stopAudioThread(phone_source* ctx);

const char *phone_source_get_name(void * /*unused*/)
{
	return "Phone Webcam";
}

// set to true to test
const bool DEBUG_SKIP_SRTP = false; // set to true to test

// ---------------------------------------------------------------------------
// Reconnect thread — unchanged from original
// ---------------------------------------------------------------------------
static void launchReconnectThread(phone_source* ctx)
{
	if (!ctx || !ctx->valid) return;
	if (ctx->reconnect_pending.exchange(true)) return;

	// Don't block — if a handshake is in progress let it finish
    if (ctx->handshake_thread.joinable()) {
        blog(LOG_INFO, "Reconnect: skipping — handshake in progress");
        ctx->reconnect_pending = false;
        return;
    }


    // Don't block — if previous reconnect thread is still running, skip
    if (ctx->reconnect_thread.joinable()) {
        blog(LOG_INFO, "Reconnect: skipping — previous reconnect still running");
        ctx->reconnect_pending = false;
        return;
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

				std::string pw;
				{
					std::lock_guard<std::mutex> lock2(ctx->password_mutex);
					pw = ctx->password;
				}

				// Stop existing SRTP sessions (if any)
				ctx->audio_srtp.stop();
				ctx->video_srtp.stop();

				// Start SRTP sessions if password is non-empty
				if (!pw.empty()) {
					ctx->audio_srtp.start(pw);
					ctx->video_srtp.start(pw);

					ctx->receiver->set_srtp_session(&ctx->video_srtp);
				} else {
					ctx->receiver->set_srtp_session(nullptr);
				}

				if (ctx->receiver->start(port)) {
					ctx->running = true;

					startAudioThread(ctx, port);
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

			// Stop existing SRTP sessions (if any)
			ctx->audio_srtp.stop();
			ctx->video_srtp.stop();

			// Start SRTP sessions if password is non-empty
			if (!pw.empty()) {
				ctx->audio_srtp.start(pw);
				ctx->video_srtp.start(pw);

				ctx->receiver->set_srtp_session(&ctx->video_srtp);
			} else {
				ctx->receiver->set_srtp_session(nullptr);
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

static void runAudioThread(phone_source* ctx)
{

	try {// Open UDP socket on video_port + 1
		SOCKET sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

		// Update the contexts' audio_socket 
		// with the socket we just defined:
		ctx->audio_socket = sock;

		int v6only = 0;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
				(const char*)&v6only, sizeof(v6only));

		int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				(const char*)&reuse, sizeof(reuse));
		

		if (sock == INVALID_SOCKET) {
			blog(LOG_ERROR, "audio thread: failed to create socket");
			return;
		}

		// Set receive timeout so we can check ctx->audio_running
		DWORD timeout_ms = 100;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
				(const char*)&timeout_ms, sizeof(timeout_ms));

		sockaddr_in6 addr{};
		addr.sin6_family = AF_INET6;
		addr.sin6_addr   = in6addr_any;
		addr.sin6_port   = htons(ctx->audio_port);

		if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
			blog(LOG_ERROR, "audio thread: failed to bind port %d", (int)ctx->audio_port);
			closesocket(sock);
			ctx->audio_socket = INVALID_SOCKET;
			return;
		}

		// Add this after bind() in runAudioThread:
		int rcvbuf = 4 * 1024 * 1024;  // 4MB is plenty for audio
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

		blog(LOG_INFO, "audio thread: listening on port %d", (int)ctx->audio_port);

		std::vector<uint8_t> buf(65536);
		std::vector<float>   pcm;

		while (ctx->audio_running && ctx->valid) {

			sockaddr_in6 sender{};
			socklen_t sender_len = sizeof(sender);

			int len = recvfrom(sock,
							(char*)buf.data(),
							(int)buf.size(),
							0,
							(sockaddr*)&sender,
							&sender_len);

			blog(LOG_INFO, "audio: recvfrom returned %d", len);

			if (len <= 0)
				continue;

			if (len <= 12)
				continue;

			// 🔐 Decrypt SRTP if active
			if (!DEBUG_SKIP_SRTP && ctx->audio_srtp.is_active()) {
				len = ctx->audio_srtp.unprotect(buf.data(), len);
				//blog(LOG_INFO, "audio: after unprotect len=%d", len); 
				if (len < 0) {
					//blog(LOG_WARNING, "audio: SRTP unprotect failed, dropping packet");
					continue;
				}
			}

			if (len <= 12)
				continue;

			// Strip 12-byte RTP header
			const uint8_t* payload = buf.data() + 12;
			int payload_len = len - 12;

			int sample_rate = 0, channels = 0;

			if (!ctx->audio_decoder->decode(payload, payload_len, pcm, sample_rate, channels))
				continue;

			if (pcm.empty() || sample_rate == 0 || channels == 0)
				continue;

			obs_source_audio audio{};
			audio.frames          = (uint32_t)(pcm.size() / channels);
			audio.samples_per_sec = (uint32_t)sample_rate;
			audio.format          = AUDIO_FORMAT_FLOAT;
			audio.speakers        = (channels == 1) ? SPEAKERS_MONO : SPEAKERS_STEREO;
			audio.timestamp       = os_gettime_ns();
			audio.data[0]         = (const uint8_t*)pcm.data();

			obs_source_output_audio(ctx->source, &audio);
		}

		closesocket(sock);
		ctx->audio_socket = INVALID_SOCKET;

		blog(LOG_INFO, "audio thread: exiting");


    } catch (const std::exception& e) {
        blog(LOG_ERROR, "audio thread EXCEPTION: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "audio thread UNKNOWN EXCEPTION");
    }
    blog(LOG_INFO, "audio thread: exited safely");
}

static void startAudioThread(phone_source* ctx, uint16_t video_port)
{
    stopAudioThread(ctx); // safety — ensure no stale thread
    ctx->audio_port    = video_port + 1;
    ctx->audio_running = true;
    ctx->audio_thread  = std::thread(runAudioThread, ctx);
}

static void stopAudioThread(phone_source* ctx)
{
	blog(LOG_INFO, "stopAudioThread: setting audio_running=false");
    ctx->audio_running = false;

    if (ctx->audio_thread.joinable()) {

		blog(LOG_INFO, "stopAudioThread: joining audio thread...");
        ctx->audio_thread.join();


        ctx->audio_thread = std::thread();
    } 
	else {
		blog(LOG_INFO, "stopAudioThread: no thread to join");
	}
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

			blog(LOG_INFO, "NAL_RECV wall=%lld", (long long)os_gettime_ns() / 1000000);
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
	// assign to all mixer tracks
	//obs_source_set_audio_mixers(ctx->source, 0x3F); // Tracks 1 through 6
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
	ctx->audio_decoder = std::make_unique<AacDecoder>();
	ctx->discovery = std::make_unique<MdnsDiscovery>();

	std::string pw;

	// Load initial password from settings
	{
		std::lock_guard<std::mutex> lock(ctx->password_mutex);
		ctx->password = obs_data_get_string(settings, "password");
	}
	
	// Start the SRTP session for the audio if password is there
	if (!ctx->password.empty()) {
    	ctx->audio_srtp.start(ctx->password);
    	ctx->video_srtp.start(ctx->password);

		ctx->receiver->set_srtp_session(&ctx->video_srtp);
	}

	pw = ctx->password;
	
	// Discovery callback
	ctx->discovery->start([ctx, pw](const std::string& ip, uint16_t port) {
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

		// Guard BEFORE starting anything
		if (ctx->handshake_thread.joinable()) {
			blog(LOG_INFO, "mDNS: skipping — previous handshake still in progress");
			ctx->discovery->resume();
			return;
		}

		ctx->port = port;
		//ctx->mdns_mode = true;

		if (!ctx->receiver->start(port)) {
			blog(LOG_ERROR, "mDNS: Failed to start receiver on port %d", (int)port);
			ctx->discovery->resume();
			return;
		}

		ctx->running = true;
		startAudioThread(ctx, port);
		ctx->network_thread = std::thread(runNetworkThread, ctx, true);

		std::string ip_copy   = ip;
		uint16_t    port_copy = port;

		// CHANGE 2: "fresh_pw"
		std::string fresh_pw;
		{
			std::lock_guard<std::mutex> lock(ctx->password_mutex);
			fresh_pw = ctx->password;
		}

		ctx->handshake_thread = std::thread([ctx, ip_copy, port_copy, fresh_pw]() {
			if (!ctx || !ctx->valid) return;

			if (!HandshakeClient::sendHandshake(ip_copy, port_copy, fresh_pw)) {
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
	});

	ctx->handshakeServer = std::make_unique<HandshakeServer>();

	// Apply initial password to handshake server
	{
		std::lock_guard<std::mutex> lock(ctx->password_mutex);
		ctx->handshakeServer->setPassword(ctx->password);
	}

	// Handshake server callback (Android→OBS direction)
	ctx->handshakeServer->start([ctx, pw](const std::string& phoneIp) {
		if (!ctx || !ctx->valid) return;

		blog(LOG_INFO, "HandshakeServer: Android requested handshake from %s", phoneIp.c_str());

		uint16_t port = ctx->port.load();
		if (port == 0) port = 9000;

		if (ctx->running) {
			ctx->running = false;
			ctx->receiver->stop();
		}

		std::string pw;
		{
			std::lock_guard<std::mutex> lock2(ctx->password_mutex);
			pw = ctx->password;
		}
		// Stop existing SRTP sessions (if any)
		ctx->audio_srtp.stop();
		ctx->video_srtp.stop();

		// Start SRTP sessions if password is non-empty
		if (!pw.empty()) {
			ctx->audio_srtp.start(pw);
			ctx->video_srtp.start(pw);

			ctx->receiver->set_srtp_session(&ctx->video_srtp);
		} else {
			ctx->receiver->set_srtp_session(nullptr);
		}

		// Capture old thread to join inside the new one — don't block the listen thread
		std::thread old_thread = std::move(ctx->handshake_thread);

		ctx->handshake_thread = std::thread([ctx, phoneIp, port, pw,
											old_thread = std::move(old_thread)]() mutable {
			// Join previous handshake thread safely from within the new thread
			if (old_thread.joinable()) old_thread.join();

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

			ctx->port      = port;
			ctx->running   = true;
			//ctx->mdns_mode = false;

			startAudioThread(ctx, port);

			blog(LOG_INFO, "HandshakeServer: startAudioThread done, launching network thread");
			ctx->network_thread = std::thread(runNetworkThread, ctx, false);
			blog(LOG_INFO, "HandshakeServer: network thread launched");

			HandshakeClient::sendHandshake(phoneIp, port, pw);
			ctx->lastIp = phoneIp;
		});
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

	stopAudioThread(ctx);
	ctx->audio_srtp.stop();
	ctx->video_srtp.stop();

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

	// Put this back now that we have a "true" default setting in phone_source_create:
    //ctx->mdns_mode = false;

	// CHANGE 3: ABSOLUTELY NECESSARY TO PREVENT STREAM LAG:
	// ----------------------------------------------------- //
	// Only act if something actually changed
    std::string old_password;
    {
        std::lock_guard<std::mutex> lock(ctx->password_mutex);
        old_password = ctx->password;
    }
    if (new_port == ctx->port.load() && new_password == old_password)
        return;  // nothing changed, don't reconnect
	// ----------------------------------------------------- //

	// Something changes - now update and reconnect:
	ctx->port    = new_port;

    // Update password atomically — handshakeServer picks it up per-connection
    {
        std::lock_guard<std::mutex> lock(ctx->password_mutex);
        ctx->password = new_password;
    }

	ctx->audio_srtp.stop();
	ctx->video_srtp.stop();

	if (!new_password.empty()) {
		ctx->audio_srtp.start(new_password);
		ctx->video_srtp.start(new_password);

		ctx->receiver->set_srtp_session(&ctx->video_srtp);
	} else {
		ctx->receiver->set_srtp_session(nullptr);
	}

    ctx->handshakeServer->setPassword(new_password);

    if (ctx->running) {
        ctx->running = false;
        ctx->receiver->stop();
		// only reconnect if actually streaming
		launchReconnectThread(ctx);
	}

	// If not running, just update the password — next stream start picks it up
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