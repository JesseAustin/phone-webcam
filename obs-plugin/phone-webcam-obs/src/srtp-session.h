#pragma once

#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <srtp3/srtp.h>

class SrtpSession {
    
public:
    SrtpSession() = default;
    ~SrtpSession() {stop();}

    // Derive 30-byte key using PBKDF2 and initialize libsrtp
    bool start(const std::string& password);
    
    //int protect(uint8_t* buf, int len);

    // In-place decryption of RTP packets
    // Returns new length, or -1 on failure
    int unprotect(uint8_t* buf, int len);

    void stop(); // stop a single SSRC

    bool is_active() const { return active_; }
    
private:

    srtp_t session_ = nullptr;
    bool active_ = false;
    // Master key member for SRTP Encryption
    uint8_t master_key_[30] = {};

};