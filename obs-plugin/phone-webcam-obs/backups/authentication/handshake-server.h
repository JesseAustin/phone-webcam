// handshake-server.h
#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

class HandshakeServer {
public:
    // Callback fires with the connecting client's IP when Android initiates
    using OnRequestReceived = std::function<void(const std::string& phoneIp)>;

    HandshakeServer() = default;
    ~HandshakeServer() { stop(); }

    void start(OnRequestReceived callback);
    void stop();

    // Update the shared secret at any time (thread-safe).
    // Empty string = no auth (accept all connections).
    // Called from phone_source_update when the OBS password field changes.
    void setPassword(const std::string& pw);

private:
    void listenLoop();

    OnRequestReceived callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Password protected by its own mutex so setPassword() can be called
    // from any thread while listenLoop() reads it per-connection.
    std::mutex  passwordMutex_;
    std::string password_;

#ifdef _WIN32
    int serverSocket_ = -1;
#endif
};