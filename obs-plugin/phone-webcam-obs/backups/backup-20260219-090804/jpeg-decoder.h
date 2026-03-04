#pragma once

#include <vector>
#include <cstdint>
#include <turbojpeg.h>

class JpegDecoder {
public:
	JpegDecoder();
	~JpegDecoder();

	// Decodes a JPEG buffer into BGRA (4 bytes/pixel) output.
	// Returns false on any decode error; width/height are set on success.
	bool decode(const std::vector<uint8_t> &jpeg_data,
	            std::vector<uint8_t>       &bgra_data,
	            uint32_t                   &width,
	            uint32_t                   &height);

private:
    tjhandle tj_ = nullptr;
};
