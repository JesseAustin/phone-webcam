#pragma once

#include "frame-decoder.h"
#include <obs-module.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class H264Decoder : public IFrameDecoder {
public:
    H264Decoder();
    ~H264Decoder() override;

    void flush() override;

    video_format outputFormat() const;

    bool decode(const std::vector<uint8_t>& input,
                std::vector<uint8_t>&       output,
                uint32_t&                   width,
                uint32_t&                   height) override;

private:
    AVCodecContext* ctx_  = nullptr;
    AVPacket*       pkt_  = nullptr;
    AVFrame*        frame_= nullptr;
};