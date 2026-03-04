#pragma once
#include <string>
#include <cstdint>

class HandshakeClient {
public:
    // Detects best local IP (IPv6 global first, IPv4 fallback)
    // then sends {"ip":"...","port":...} to phoneIp:9001
    static bool sendHandshake(const std::string& phoneIp,
                              uint16_t streamPort);

private:
    static std::string getBestLocalIp();
};