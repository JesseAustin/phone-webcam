#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>

// Callback fired when the phone is found on the network.
// ip = resolved IP address (IPv4 or IPv6), port = streaming port
using DiscoveryCallback = std::function<void(const std::string& ip, uint16_t port)>;

class MdnsDiscovery {
public:
    MdnsDiscovery();
    ~MdnsDiscovery();

    // Start browsing for _phonewebcam._udp.local
    // Calls callback on the discovery thread when phone is found
    void start(DiscoveryCallback callback);
    void stop();
    
    // Pause discovery queries (but keep thread alive for resume)
    void pause();
    // Resume querying
    void resume();

    void resetDiscoveryState();

    //bool isRunning() const { return running_; }

private:
    void discoveryLoop();

    std::thread         discovery_thread_;
    std::atomic<bool>   running_{ false };
    std::atomic<bool>   paused_{ false };
    DiscoveryCallback   callback_;
    
    // Track the last seen service to avoid duplicate callbacks
    std::string         last_service_hostname_;
    uint16_t            last_service_port_{ 0 };
    // Time when the last service was accepted (used for debounce)
    std::chrono::steady_clock::time_point last_service_time_;
};