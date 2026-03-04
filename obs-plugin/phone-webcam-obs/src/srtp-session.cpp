#include "srtp-session.h"
#include <openssl/evp.h>
#include <cstring>
#include <string>
#include <obs-module.h>

bool SrtpSession::start(const std::string& password) {
    stop();
    if (password.empty()) return false;

    static bool srtp_initialized = false;
    if (!srtp_initialized) {
        srtp_err_status_t init_status = srtp_init();
        if (init_status != srtp_err_status_ok) {
            blog(LOG_ERROR, "SRTP: srtp_init failed: %d", (int)init_status);
            return false;
        }
        srtp_initialized = true;
    }

    const char* salt = "PhoneWebcamSRTP";
    if (PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.length(),
                          (const unsigned char*)salt, (int)strlen(salt),
                          1000, EVP_sha256(), 30, master_key_) != 1) {
        blog(LOG_ERROR, "SRTP: PBKDF2 key derivation failed");
        return false;
    }

    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    policy.ssrc.type       = ssrc_any_inbound;
    policy.ssrc.value      = 0;
    policy.key             = master_key_;
    // much more forgiving for audio
    policy.window_size = 1024;  
    policy.allow_repeat_tx = 1;
    policy.next            = nullptr;

    srtp_err_status_t status = srtp_create(&session_, &policy);
    if (status != srtp_err_status_ok) {
        blog(LOG_ERROR, "SRTP: srtp_create failed: %d", (int)status);
        return false;
    }

    active_ = true;
    blog(LOG_INFO, "SRTP: session initialized");
    return true;
}

int SrtpSession::unprotect(uint8_t* buf, int len) {
    if (!active_ || !session_) return -1;

    size_t out_len = (size_t)len;
    srtp_err_status_t status = srtp_unprotect(session_, buf, (size_t)len, buf, &out_len);
    if (status != srtp_err_status_ok) {
        blog(LOG_WARNING, "SRTP: unprotect failed: %d", (int)status);
        return -1;
    }
    return (int)out_len;
}

void SrtpSession::stop() {
    if (session_) {
        srtp_dealloc(session_);
        session_ = nullptr;
    }
    active_ = false;
}