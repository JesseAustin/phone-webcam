// handshake-server.h
#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>

class HandshakeServer {
public:
    // Callback fires with the connecting client's IP when Android initiates
    using OnRequestReceived = std::function<void(const std::string& phoneIp)>;

    HandshakeServer() = default;
    ~HandshakeServer() { stop(); }

    void start(OnRequestReceived callback);
    void stop();

private:
    void listenLoop();

    OnRequestReceived callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};

#ifdef _WIN32
    int serverSocket_ = -1;
#endif
};