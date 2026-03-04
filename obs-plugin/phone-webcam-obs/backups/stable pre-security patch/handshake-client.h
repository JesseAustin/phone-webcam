#pragma once
#include <string>
#include <cstdint>

class HandshakeClient {
public:

    static bool sendHandshake(const std::string& phoneIp,
                              uint16_t streamPort);

private:
    static std::string getBestLocalIp();
};