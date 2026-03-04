#include <jni.h>
#include <turbojpeg.h>
#include <android/log.h>
#include <cstdlib>
#include <cstring>

#define TAG "TurboJpegJNI"

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_phone_webcam_phonewebcam_encoder_TurboJpegEncoder_compressI420(
        JNIEnv *env,
        jobject,
        jbyteArray yBuf,
        jbyteArray uBuf,
        jbyteArray vBuf,
        jint yRowStride,
        jint uvRowStride,
        jint uvPixelStride,
        jint width,
        jint height,
        jint quality)
{
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "tj3Init failed");
        return nullptr;
    }

    jbyte *y = env->GetByteArrayElements(yBuf, nullptr);
    jbyte *u = env->GetByteArrayElements(uBuf, nullptr);
    jbyte *v = env->GetByteArrayElements(vBuf, nullptr);

    const unsigned char *srcPlanes[3];
    int strides[3];

    // Y plane is always straightforward
    srcPlanes[0] = reinterpret_cast<const unsigned char*>(y);
    strides[0]   = yRowStride;

    int chromaW = width  / 2;
    int chromaH = height / 2;

    unsigned char *uPlanar = nullptr;
    unsigned char *vPlanar = nullptr;

    if (uvPixelStride == 1) {
        // Already fully planar I420 — pass directly, no copy needed
        srcPlanes[1] = reinterpret_cast<const unsigned char*>(u);
        srcPlanes[2] = reinterpret_cast<const unsigned char*>(v);
        strides[1]   = uvRowStride;
        strides[2]   = uvRowStride;
    } else {
        // pixelStride == 2: semi-planar (NV12/NV21 style interleaved)
        // De-interleave into separate planar buffers for TurboJPEG
        uPlanar = new unsigned char[chromaW * chromaH];
        vPlanar = new unsigned char[chromaW * chromaH];

        const unsigned char *uSrc = reinterpret_cast<const unsigned char*>(u);
        const unsigned char *vSrc = reinterpret_cast<const unsigned char*>(v);

        for (int row = 0; row < chromaH; row++) {
            for (int col = 0; col < chromaW; col++) {
                uPlanar[row * chromaW + col] = uSrc[row * uvRowStride + col * 2];
                vPlanar[row * chromaW + col] = vSrc[row * uvRowStride + col * 2];
            }
        }

        srcPlanes[1] = uPlanar;
        srcPlanes[2] = vPlanar;
        strides[1]   = chromaW;
        strides[2]   = chromaW;
    }

    tj3Set(tj, TJPARAM_SUBSAMP,  TJSAMP_420);
    tj3Set(tj, TJPARAM_QUALITY,  quality);
    tj3Set(tj, TJPARAM_FASTDCT,  1);

    unsigned char *jpegBuf  = nullptr;
    size_t         jpegSize = 0;

    int ret = tj3CompressFromYUVPlanes8(
        tj,
        srcPlanes,
        width,
        strides,
        height,
        &jpegBuf,
        &jpegSize
    );

    env->ReleaseByteArrayElements(yBuf, y, JNI_ABORT);
    env->ReleaseByteArrayElements(uBuf, u, JNI_ABORT);
    env->ReleaseByteArrayElements(vBuf, v, JNI_ABORT);
    delete[] uPlanar;
    delete[] vPlanar;

    jbyteArray result = nullptr;
    if (ret == 0 && jpegBuf && jpegSize > 0) {
        result = env->NewByteArray((jsize)jpegSize);
        env->SetByteArrayRegion(result, 0, (jsize)jpegSize,
                                reinterpret_cast<const jbyte*>(jpegBuf));
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "compress failed: %s",
                            tj3GetErrorStr(tj));
    }

    tj3Free(jpegBuf);
    tj3Destroy(tj);
    return result;
}