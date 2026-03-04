// handshake-server.cpp
#define NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "handshake-server.h"
#include "auth.h"
#include <obs-module.h>

#ifdef _WIN32
#include <wincrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#include <string>
#include <sstream>

static constexpr uint16_t HANDSHAKE_SERVER_PORT = 9001;

// ---------------------------------------------------------------------------
// Minimal JSON string extractor — avoids pulling in a JSON library.
// Finds the value of "key" in a flat JSON object string.
// Only handles string and integer values (no nesting needed here).
// ---------------------------------------------------------------------------
static std::string jsonGetString(const std::string& json, const std::string& key)
{
    // Look for: "key":"value"
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------

void HandshakeServer::start(OnRequestReceived callback)
{
    callback_ = callback;
    running_ = true;
    thread_ = std::thread(&HandshakeServer::listenLoop, this);
}

void HandshakeServer::stop()
{
    running_ = false;
#ifdef _WIN32
    if (serverSocket_ != -1) {
        closesocket(serverSocket_);
        serverSocket_ = -1;
    }
#endif
    if (thread_.joinable())
        thread_.join();
}

void HandshakeServer::setPassword(const std::string& pw)
{
    std::lock_guard<std::mutex> lock(passwordMutex_);
    password_ = pw;
}

void HandshakeServer::listenLoop()
{
#ifdef _WIN32
    serverSocket_ = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket_ == -1) {
        blog(LOG_ERROR, "HandshakeServer: socket creation failed");
        return;
    }

    int reuse = 1;
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HANDSHAKE_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        blog(LOG_ERROR, "HandshakeServer: bind failed on port %d: %d",
             HANDSHAKE_SERVER_PORT, WSAGetLastError());
        closesocket(serverSocket_);
        serverSocket_ = -1;
        return;
    }

    listen(serverSocket_, 4);
    blog(LOG_INFO, "HandshakeServer: listening on port %d", HANDSHAKE_SERVER_PORT);

    while (running_) {
        sockaddr_in clientAddr = {};
        int clientLen = sizeof(clientAddr);

        SOCKET client = accept(serverSocket_, (sockaddr*)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET) {
            if (running_)
                blog(LOG_WARNING, "HandshakeServer: accept failed: %d", WSAGetLastError());
            break;
        }

        char phoneIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, phoneIp, sizeof(phoneIp));

        char buf[512] = {};
        int bytes = recv(client, buf, sizeof(buf) - 1, 0);
        closesocket(client);

        if (bytes <= 0) continue;

        buf[bytes] = '\0';
        std::string msg(buf);
        blog(LOG_DEBUG, "HandshakeServer: received from %s: %s", phoneIp, buf);

        // Must be a "request" message from Android
        if (msg.find("request") == std::string::npos) {
            blog(LOG_DEBUG, "HandshakeServer: ignored non-request data from %s", phoneIp);
            continue;
        }

        // ------------------------------------------------------------------
        // Authentication check
        //
        // Android sends:  {"request":"handshake","challenge":"<hex>","hmac":"<hex>"}
        //
        // If OBS has a password set:
        //   - Extract challenge and hmac from the JSON
        //   - Recompute HMAC-SHA256(password, challenge_bytes)
        //   - Reject if they don't match or fields are missing
        //
        // If OBS password is empty: accept all connections (backward compat)
        // ------------------------------------------------------------------
        {
            std::lock_guard<std::mutex> lock(passwordMutex_);

            if (!password_.empty()) {
                std::string challengeHex = jsonGetString(msg, "challenge");
                std::string receivedMac  = jsonGetString(msg, "hmac");

                if (challengeHex.empty() || receivedMac.empty()) {
                    blog(LOG_WARNING,
                         "HandshakeServer: rejected %s — no auth token (password is set)",
                         phoneIp);
                    continue;
                }

                // Decode hex challenge
                auto challengeBytes = Auth::fromHex(challengeHex);
                if (challengeBytes.empty()) {
                    blog(LOG_WARNING, "HandshakeServer: rejected %s — bad challenge hex", phoneIp);
                    continue;
                }

                // Recompute expected HMAC
                auto expected = Auth::hmac_sha256(password_, challengeBytes);
                std::string expectedHex = Auth::toHex(expected);

                if (!Auth::constTimeEqual(expectedHex, receivedMac)) {
                    blog(LOG_WARNING,
                         "HandshakeServer: rejected %s — HMAC mismatch (wrong password)",
                         phoneIp);
                    continue;  // Silently drop — stream simply doesn't start
                }

                blog(LOG_INFO, "HandshakeServer: auth OK for %s", phoneIp);
            }
        }

        // Auth passed (or no password set) — fire callback
        blog(LOG_INFO, "HandshakeServer: valid request from %s", phoneIp);
        if (running_ && callback_)
            callback_(std::string(phoneIp));
    }

    closesocket(serverSocket_);
    serverSocket_ = -1;
#endif
}