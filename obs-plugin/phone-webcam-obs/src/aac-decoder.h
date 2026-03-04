#pragma once
#include <vector>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();

    // Feed one RTP payload (ADTS + AAC), get back interleaved float32 PCM
    bool decode(const uint8_t* data, size_t size,
                std::vector<float>& pcm_out, int& sample_rate, int& channels);

private:
    AVCodecContext* ctx_ = nullptr;
    AVPacket*       pkt_ = nullptr;
    AVFrame*        frm_ = nullptr;
};