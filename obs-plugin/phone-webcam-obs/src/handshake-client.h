#pragma once
#include <string>
#include <cstdint>

class HandshakeClient {
public:
    // Send our IP+port to the phone so it knows where to stream.
    // If password is non-empty, a challenge+HMAC is included for the phone
    // to verify before it starts sending RTP.
    // If password is empty, behaves exactly as before (backward compatible).
    static bool sendHandshake(const std::string& phoneIp,
                              uint16_t streamPort,
                              const std::string& password = "");

private:
    static std::string getBestLocalIp();
};