#pragma once

#include "frame-decoder.h"
#include <turbojpeg.h>
#include <obs-module.h>

class JpegDecoder : public IFrameDecoder {
public:

    JpegDecoder();
    ~JpegDecoder() override;

    video_format outputFormat() const override { return VIDEO_FORMAT_BGRA; }

    // Decodes a JPEG buffer into BGRA (4 bytes/pixel) output.
    // Returns false on any decode error; width/height are set on success.
    bool decode(const std::vector<uint8_t>& input,
                std::vector<uint8_t>&       output,
                uint32_t&                   width,
                uint32_t&                   height) override;

private:
    tjhandle tj_ = nullptr;
};
