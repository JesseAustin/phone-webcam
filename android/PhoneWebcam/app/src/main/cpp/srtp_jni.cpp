#include <jni.h>
#include <srtp.h>
#include <android/log.h>
#include <cstring>
#include <mutex>

#define TAG "SRTP_JNI"

struct SrtpSession {
    srtp_t session = nullptr;
    std::mutex mutex;
};

static jfieldID getNativePtrField(JNIEnv *env, jobject obj) {
    jclass cls = env->GetObjectClass(obj);
    return env->GetFieldID(cls, "nativePtr", "J");
}

static SrtpSession* getSession(JNIEnv *env, jobject obj) {
    jfieldID fid = getNativePtrField(env, obj);
    return reinterpret_cast<SrtpSession*>(env->GetLongField(obj, fid));
}

extern "C"
JNIEXPORT jint JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_init(
        JNIEnv *env, jobject thiz,
        jbyteArray masterKey, jbyteArray masterSalt, jint ssrc)
{
    static bool srtp_initialized = false;
    if (!srtp_initialized) {
        srtp_init();
        srtp_initialized = true;
    }

    jfieldID fid = getNativePtrField(env, thiz);
    SrtpSession* s = reinterpret_cast<SrtpSession*>(env->GetLongField(thiz, fid));
    if (!s) {
        s = new SrtpSession();
        env->SetLongField(thiz, fid, reinterpret_cast<jlong>(s));
    }

    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->session) {
        srtp_dealloc(s->session);
        s->session = nullptr;
    }

    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.ssrc.type = ssrc_any_outbound;
    policy.ssrc.value = 0;
    policy.window_size = 1024;
    policy.allow_repeat_tx = 1;
    srtp_crypto_policy_set_rtp_default(&policy.rtp);

    uint8_t key_material[30];
    jbyte* k = env->GetByteArrayElements(masterKey, nullptr);
    jbyte* s2 = env->GetByteArrayElements(masterSalt, nullptr);
    memcpy(key_material, k, 16);
    memcpy(key_material + 16, s2, 14);
    policy.key = key_material;
    policy.next = nullptr;

    srtp_err_status_t status = srtp_create(&s->session, &policy);
    __android_log_print(ANDROID_LOG_INFO, TAG, "srtp_create status=%d ssrc=0x%08x",
                        (int)status, (uint32_t)ssrc);

    env->ReleaseByteArrayElements(masterKey, k, JNI_ABORT);
    env->ReleaseByteArrayElements(masterSalt, s2, JNI_ABORT);
    return (jint)status;
}

extern "C"
JNIEXPORT jint JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_protect(
        JNIEnv *env, jobject thiz, jbyteArray packet, jint length)
{
    SrtpSession* s = getSession(env, thiz);
    if (!s) return -1;

    std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->session) return -1;

    jbyte* data = env->GetByteArrayElements(packet, nullptr);
    size_t rtp_len = (size_t)length;
    size_t srtp_len = rtp_len + 128;

    srtp_err_status_t status = srtp_protect(
            s->session,
            reinterpret_cast<uint8_t*>(data), rtp_len,
            reinterpret_cast<uint8_t*>(data), &srtp_len, 0);

    env->ReleaseByteArrayElements(packet, data, 0);

    if (status != srtp_err_status_ok) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "srtp_protect failed: %d", status);
        return -((jint)status);
    }
    return (jint)srtp_len;
}

extern "C"
JNIEXPORT void JNICALL
Java_phone_webcam_phonewebcam_network_SrtpContext_closeNative(JNIEnv *env, jobject thiz) {
    jfieldID fid = getNativePtrField(env, thiz);
    SrtpSession* s = reinterpret_cast<SrtpSession*>(env->GetLongField(thiz, fid));
    if (s) {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->session) {
            srtp_dealloc(s->session);
            s->session = nullptr;
        }
        env->SetLongField(thiz, fid, 0);
        // Can't delete s while holding its own mutex — unlock first
    }
    delete s;
}