#include "jpeg-decoder.h"
#include <turbojpeg.h>
#include <obs-module.h>
#include <cstring>

// TurboJPEG constructor
JpegDecoder::JpegDecoder()
{
    tj_ = tj3Init(TJINIT_DECOMPRESS);
    tj3Set(tj_, TJPARAM_FASTDCT, 1);
    if (!tj_)
        blog(LOG_ERROR, "TurboJPEG: tj3Init failed");
}

// TurboJPEG deconstructor
JpegDecoder::~JpegDecoder()
{
    if (tj_) {
        tj3Destroy(tj_);
        tj_ = nullptr;
    }
}


//TurboJPEG decoding  function
bool JpegDecoder::decode(const std::vector<uint8_t> &jpeg_data,
                         std::vector<uint8_t>       &bgra_data,
                         uint32_t                   &width,
                         uint32_t                   &height)
{
    if (!tj_ || jpeg_data.empty()) return false;

    if (tj3DecompressHeader(tj_, jpeg_data.data(), jpeg_data.size()) < 0) {
        blog(LOG_WARNING, "TurboJPEG: header read failed: %s", tj3GetErrorStr(tj_));
        return false;
    }

    width  = (uint32_t)tj3Get(tj_, TJPARAM_JPEGWIDTH);
    height = (uint32_t)tj3Get(tj_, TJPARAM_JPEGHEIGHT);
    if (width == 0 || height == 0) return false;

    bgra_data.resize(width * height * 4);

    int ret = tj3Decompress8(tj_,
                  jpeg_data.data(),
                  jpeg_data.size(),
                  bgra_data.data(),
                  0,
                  TJPF_BGRA);

    if (ret < 0) {
        blog(LOG_WARNING, "TurboJPEG: decode failed: %s", tj3GetErrorStr(tj_));
        return false;
    }
    return true;
}
