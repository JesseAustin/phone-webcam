#include <jni.h>
#include <srtp.h>
#include <android/log.h>
#include <cstring>
#include <vector>

#define TAG "SRTP_JNI"

static srtp_t session = nullptr;
static uint32_t session_ssrc = 0;

extern "C"
JNIEXPORT jint JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_init(
    JNIEnv *env,
    jobject,
    jbyteArray masterKey,
    jbyteArray masterSalt,
    jint ssrc
    )
{
    // INIT BLOCK: Initialize libsrtp (safe to call multiple times)
    static bool srtp_initialized = false;

    if (!srtp_initialized) {
        srtp_err_status_t status = srtp_init();
        if (status != srtp_err_status_ok) {
            return (jint)status;
        }
        srtp_initialized = true;
    }

    // Cleanup old session if it exists
    if (session) {
        srtp_dealloc(session);
        session = nullptr;
    }

    srtp_policy_t policy;
    memset(&policy, 0, sizeof(srtp_policy_t));

    policy.ssrc.type  = ssrc_any_outbound;
    policy.ssrc.value = 0;
    policy.window_size = 1024;
    policy.allow_repeat_tx = 1;  // important — you retransmit NALs on reconnect

    // Configure for AES-ICM 128-bit (Standard for SRTP)
    srtp_crypto_policy_set_rtp_default(&policy.rtp);

    // Combine key and salt into libsrtp's expected format (30 bytes for AES-128)
    // Key (16 bytes) + Salt (14 bytes)
    uint8_t key_material[30];

    jbyte* k = env->GetByteArrayElements(masterKey, nullptr);
    jbyte* s = env->GetByteArrayElements(masterSalt, nullptr);

    memcpy(key_material, k, 16);
    memcpy(key_material + 16, s, 14);

    policy.key = key_material;
    policy.next = nullptr;

    srtp_err_status_t status = srtp_create(&session, &policy);

    session_ssrc = (uint32_t)ssrc;
    __android_log_print(ANDROID_LOG_INFO, TAG, "srtp_create status=%d ssrc=0x%08x",
                        (int)status, session_ssrc);

    env->ReleaseByteArrayElements(masterKey, k, JNI_ABORT);
    env->ReleaseByteArrayElements(masterSalt, s, JNI_ABORT);

    return (jint)status;
}

extern "C"
JNIEXPORT jint JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_protect(
    JNIEnv *env,
    jobject,
    jbyteArray packet,
    jint length)
{
    if (!session) return -1;

    jbyte* data = env->GetByteArrayElements(packet, nullptr);

    // We use in-place protection (rtp == srtp)
    size_t rtp_len = (size_t)length;
    size_t srtp_len = rtp_len + 128; // 128 is plenty for SRTP trailer

    // libsrtp3 srtp_protect signature:
    // srtp_err_status_t srtp_protect(srtp_t ctx, const uint8_t *rtp, size_t rtp_len,
    //                               uint8_t *srtp, size_t *srtp_len, size_t mki_index);
    srtp_err_status_t status = srtp_protect(
            session,
            reinterpret_cast<uint8_t*>(data),
            rtp_len,
            reinterpret_cast<uint8_t*>(data),
            &srtp_len,  // ← capacity of output buffer going in, actual length coming out
            0
    );

    // Mode 0 means copy back changes to the Java array.
    env->ReleaseByteArrayElements(packet, data, 0);

    if (status != srtp_err_status_ok) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "srtp_protect failed: %d", status);
        return -((jint)status);
    }

    // CRITICAL: We must return the new length (rtp_len + tag_len),
    // otherwise the Java side will send a truncated packet without the auth tag.
    return (jint)srtp_len;
}

extern "C"
JNIEXPORT void JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_closeNative(JNIEnv *, jobject) {
    if (session) {
        srtp_dealloc(session);
        session = nullptr;
    }
}