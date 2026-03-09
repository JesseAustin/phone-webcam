#include "h264-decoder.h"
#include <obs-module.h>
#include <cstring>
#include <algorithm>

bool bgraToggle = false; // Set to true to enable BGRA output (for testing)

H264Decoder::H264Decoder()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        blog(LOG_ERROR, "H264Decoder: could not find H.264 decoder");
        return;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        blog(LOG_ERROR, "H264Decoder: could not allocate codec context");
        return;
    }

    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->thread_count = 1; 
        ctx_->thread_type  = 0; 

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        blog(LOG_ERROR, "H264Decoder: could not open codec");
        avcodec_free_context(&ctx_);
        return;
    }

    pkt_   = av_packet_alloc();
    frame_ = av_frame_alloc();

    if (!pkt_ || !frame_) {
        blog(LOG_ERROR, "H264Decoder: could not allocate packet/frame");
        return;
    }

    blog(LOG_INFO, "H264Decoder: initialized");

    
}

H264Decoder::~H264Decoder()
{
    if (sws_) sws_freeContext(sws_);
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&ctx_);
}

video_format H264Decoder::outputFormat() const
{
    if(bgraToggle)
    {
        return VIDEO_FORMAT_BGRA;
    }
    else
    {
        return VIDEO_FORMAT_I420;
    }
}

void H264Decoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

bool H264Decoder::decode(const std::vector<uint8_t>& input,
                         std::vector<uint8_t>&        output,
                         uint32_t&                    width,
                         uint32_t&                    height)
{
    if (!ctx_ || !pkt_ || !frame_ || input.empty())
        return false;

    av_packet_unref(pkt_);
    if (av_new_packet(pkt_, static_cast<int>(input.size())) < 0)
        return false;

    std::memcpy(pkt_->data, input.data(), input.size());
    std::memset(pkt_->data + input.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt_->size = static_cast<int>(input.size());

    int send_ret = avcodec_send_packet(ctx_, pkt_);
    if (send_ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(send_ret, errbuf, sizeof(errbuf));
        blog(LOG_WARNING, "H264Decoder: avcodec_send_packet failed: %s", errbuf);
        return false;
    }

    int ret = avcodec_receive_frame(ctx_, frame_);

    // Return TRUE but set width/height to 0 so the renderer knows to skip this "frame".
    if (ret == AVERROR(EAGAIN)) {
        width = 0;
        height = 0;
        return true; 
    }
    if (ret == AVERROR_EOF)
        return false;
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        blog(LOG_WARNING, "H264Decoder: avcodec_receive_frame failed: %s", errbuf);
        return false;
    }

    // No errors, we got a frame! Now convert it to the desired output format if needed.
    if (ret == 0) {
        // Check if the hardware resolution actually changed
        if (frame_->width != ctx_->width || frame_->height != ctx_->height) {
            blog(LOG_INFO, "Resolution change detected: %dx%d -> %dx%d", 
                ctx_->width, ctx_->height, frame_->width, frame_->height);
            
            // This is the safest way to ensure the next frame isn't garbled
            avcodec_flush_buffers(ctx_);
        }
        if (bgraToggle)  // Using BGRA output for testing
        {
            width  = static_cast<uint32_t>(frame_->width);
            height = static_cast<uint32_t>(frame_->height);
            output.resize(width * height * 4);

            // Recreate SwsContext only if dimensions or format changed
            if (!sws_ || sws_src_width_ != frame_->width || 
                sws_src_height_ != frame_->height || 
                sws_src_fmt_ != (AVPixelFormat)frame_->format) {
                if (sws_) sws_freeContext(sws_);
                sws_ = sws_getContext(
                    frame_->width, frame_->height, (AVPixelFormat)frame_->format,
                    frame_->width, frame_->height, AV_PIX_FMT_BGRA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                sws_src_width_  = frame_->width;
                sws_src_height_ = frame_->height;
                sws_src_fmt_    = (AVPixelFormat)frame_->format;
            }

            uint8_t* dst[1]   = { output.data() };
            int dst_stride[1] = { (int)width * 4 };
            sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dst_stride);
        }

        else {  // Using I420 output for production
            width  = static_cast<uint32_t>(frame_->width);
            height = static_cast<uint32_t>(frame_->height);

            const size_t y_size  = (size_t)width * height;
            const size_t u_size  = y_size / 4;
            output.resize(y_size + u_size * 2);

            // Copy Y plane row by row to handle FFmpeg's alignment padding
            for (uint32_t row = 0; row < height; row++) {
                memcpy(output.data() + row * width,
                    frame_->data[0] + row * frame_->linesize[0],
                    width);
            }
            // Copy U plane
            for (uint32_t row = 0; row < height / 2; row++) {
                memcpy(output.data() + y_size + row * (width / 2),
                    frame_->data[1] + row * frame_->linesize[1],
                    width / 2);
            }
            // Copy V plane
            for (uint32_t row = 0; row < height / 2; row++) {
                memcpy(output.data() + y_size + u_size + row * (width / 2),
                    frame_->data[2] + row * frame_->linesize[2],
                    width / 2);
            }

            /*
            blog(LOG_INFO, "I420 copy done: y_size=%zu u_size=%zu total=%zu buf_size=%zu",
            y_size, u_size, y_size + u_size * 2, output.size());
            blog(LOG_INFO, "I420 Y[0]=%02X U[0]=%02X V[0]=%02X",
            output[0], output[y_size], output[y_size + u_size]);
            */
        }
    }
    av_frame_unref(frame_);
    return true;
}