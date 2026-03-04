#pragma once

#include <vector>
#include <cstdint>
#include <obs-module.h>

// Abstract interface for frame decoders.
// Both JpegDecoder and H264Decoder implement this so phone-source.cpp
// can swap between them without knowing which is active.
class IFrameDecoder {
public:
    virtual ~IFrameDecoder() = default;

    virtual video_format outputFormat() const = 0;

    // In frame-decoder.h
    virtual void flush() {}


    // Decode a compressed input buffer into raw BGRA output.
    // Returns false on any decode error.
    // On success, width and height are set and output is resized to fit.
    virtual bool decode(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>&       output,
                        uint32_t&                   width,
                        uint32_t&                   height) = 0;
};
