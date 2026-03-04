#include "aac-decoder.h"
#include <obs-module.h>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

AacDecoder::AacDecoder()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) { blog(LOG_ERROR, "aac-decoder: AAC codec not found"); return; }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) { blog(LOG_ERROR, "aac-decoder: failed to alloc context"); return; }

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        blog(LOG_ERROR, "aac-decoder: failed to open codec");
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
        return;
    }

    pkt_ = av_packet_alloc();
    frm_ = av_frame_alloc();
}

AacDecoder::~AacDecoder()
{
    av_frame_free(&frm_);
    av_packet_free(&pkt_);
    avcodec_free_context(&ctx_);
}

bool AacDecoder::decode(const uint8_t* data, size_t size,
                        std::vector<float>& pcm_out, int& sample_rate, int& channels)
{
    if (!ctx_ || !pkt_ || !frm_) return false;

    pkt_->data = const_cast<uint8_t*>(data);
    pkt_->size = (int)size;

    if (avcodec_send_packet(ctx_, pkt_) < 0) return false;
    if (avcodec_receive_frame(ctx_, frm_) < 0) return false;

    sample_rate = frm_->sample_rate;
    channels    = frm_->ch_layout.nb_channels;
    int samples = frm_->nb_samples;

    pcm_out.resize(samples * channels);

    // Convert whatever format FFmpeg gives us to float32
    if (ctx_->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        // Planar float — interleave
        for (int ch = 0; ch < channels; ch++) {
            float* plane = (float*)frm_->data[ch];
            for (int i = 0; i < samples; i++)
                pcm_out[i * channels + ch] = plane[i];
        }
    } else if (ctx_->sample_fmt == AV_SAMPLE_FMT_FLT) {
        memcpy(pcm_out.data(), frm_->data[0], samples * channels * sizeof(float));
    } else if (ctx_->sample_fmt == AV_SAMPLE_FMT_S16) {
        int16_t* src = (int16_t*)frm_->data[0];
        for (int i = 0; i < samples * channels; i++)
            pcm_out[i] = src[i] / 32768.0f;
    } else {
        blog(LOG_WARNING, "aac-decoder: unhandled sample fmt %d", ctx_->sample_fmt);
        return false;
    }

    return true;
}