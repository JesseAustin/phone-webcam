// handshake-server.cpp
#define NOMINMAX
#include "handshake-server.h"
#include <obs-module.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

static constexpr uint16_t HANDSHAKE_SERVER_PORT = 9001;

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

void HandshakeServer::listenLoop()
{
#ifdef _WIN32
    serverSocket_ = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket_ == -1) {
        blog(LOG_ERROR, "HandshakeServer: socket creation failed");
        return;
    }

    // Allow rapid restart without waiting for TIME_WAIT
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

        char buf[256] = {};
        int bytes = recv(client, buf, sizeof(buf) - 1, 0);
        
        if (bytes > 0) {
            buf[bytes] = '\0'; // Ensure null-termination for string search
            std::string msg(buf);
            
            // Only trigger if it's a specific request
            if (msg.find("request") != std::string::npos) {
                blog(LOG_INFO, "HandshakeServer: valid request from %s", phoneIp);
                if (running_ && callback_)
                    callback_(std::string(phoneIp));
            } else {
                blog(LOG_DEBUG, "HandshakeServer: ignored junk data from %s", phoneIp);
            }
        }
    closesocket(client); // Close immediately after reading
}

    closesocket(serverSocket_);
    serverSocket_ = -1;
#endif
}