// FFmpegStreamer.cpp
#include "streamer.h"
#include <iostream>

extern "C" {
#include <libavutil/pixdesc.h>
}

static std::string ffErr2Str(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

static AVPixelFormat pickEncoderPixFmt(const AVCodec *codec, AVPixelFormat inFmt) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void *cfg = nullptr;
    int cfgCount = 0;
    int rc = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &cfgCount);
    if (rc < 0 || !cfg || cfgCount <= 0) return AV_PIX_FMT_YUV420P;

    const auto *pixFmts = static_cast<const AVPixelFormat *>(cfg);
    AVPixelFormat preferred = pixFmts[0];
    for (int i = 0; i < cfgCount; ++i) {
        const AVPixelFormat pf = pixFmts[i];
        if (pf == inFmt) return pf;
        if (pf == AV_PIX_FMT_NV12) preferred = pf;
    }
    return preferred;
#else
    const AVPixelFormat *pixFmts = codec->pix_fmts;
    if (!pixFmts) return AV_PIX_FMT_YUV420P;

    AVPixelFormat preferred = pixFmts[0];
    for (const AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == inFmt) return *p;
        if (*p == AV_PIX_FMT_NV12) preferred = *p;
    }
    return preferred;
#endif
}

FFmpegStreamer::FFmpegStreamer() {
    // Init libavformat/network if necessary
    avformat_network_init();
}

FFmpegStreamer::~FFmpegStreamer() {
    close();
    avformat_network_deinit();
}

bool FFmpegStreamer::open(const std::string &url, int width, int height, AVPixelFormat in_pixfmt, int fps) {
    url_ = url;
    width_ = width;
    height_ = height;
    fps_ = fps;
    in_pix_fmt_ = in_pixfmt;
    pts_counter_ = 0;

    // allocate output context
    avformat_alloc_output_context2(&ofmt_ctx_, nullptr, "rtsp", url.c_str());
    if (!ofmt_ctx_) {
        std::cerr << "Could not allocate output context\n";
        return false;
    }

    // Prefer Pi hardware encoder first, then fallback to software H.264.
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_v4l2m2m");
    bool using_hw = (codec != nullptr);

    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        std::cerr << "H.264 encoder not found\n";
        return false;
    }


    enc_pix_fmt_ = pickEncoderPixFmt(codec, in_pix_fmt_);
    const char *in_name = av_get_pix_fmt_name(in_pix_fmt_);
    const char *enc_name = av_get_pix_fmt_name(enc_pix_fmt_);
    std::cerr << "[RTSP] Encoder selected: " << codec->name
              << (using_hw ? " (hardware)" : " (software fallback)") << "\n"
              << "[RTSP] PixFmt input=" << (in_name ? in_name : "unknown")
              << " encoder=" << (enc_name ? enc_name : "unknown") << "\n";

    enc_ctx_ = avcodec_alloc_context3(codec);
    if (!enc_ctx_) {
        std::cerr << "Could not alloc encoder context\n";
        return false;
    }

    enc_ctx_->width = width_;
    enc_ctx_->height = height_;
    enc_ctx_->time_base = AVRational{1, fps_};
    enc_ctx_->framerate = AVRational{fps_, 1};
    enc_ctx_->gop_size = fps_; // 1 second gop
    enc_ctx_->max_b_frames = 0;
    enc_ctx_->pix_fmt = enc_pix_fmt_;
    enc_ctx_->bit_rate = 400000; // adjust

    // set encoder options: preset & tune for low-latency
    AVDictionary *codec_opts = nullptr;
    if (!using_hw) {
        av_dict_set(&codec_opts, "preset", "veryfast", 0);
        av_dict_set(&codec_opts, "tune", "zerolatency", 0);
    }

    if (avcodec_open2(enc_ctx_, codec, &codec_opts) < 0) {
        std::cerr << "Could not open encoder\n";
        av_dict_free(&codec_opts);
        return false;
    }
    av_dict_free(&codec_opts);

    // create new stream in format context
    out_stream_ = avformat_new_stream(ofmt_ctx_, nullptr);
    if (!out_stream_) {
        std::cerr << "Could not create stream\n";
        return false;
    }
    out_stream_->time_base = enc_ctx_->time_base;
    if (avcodec_parameters_from_context(out_stream_->codecpar, enc_ctx_) < 0) {
        std::cerr << "Could not copy codec parameters\n";
        return false;
    }

    // open IO (RTSP/RTSPS)
    if (!(ofmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary *io_opts = nullptr;
        av_dict_set(&io_opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&io_opts, "stimeout", "5000000", 0); // 5s in microseconds
        int io_ret = avio_open2(&ofmt_ctx_->pb, url_.c_str(), AVIO_FLAG_WRITE, nullptr, &io_opts);
        av_dict_free(&io_opts);
        if (io_ret < 0) {
            std::cerr << "Could not open output URL: " << url_
                      << " err=" << ffErr2Str(io_ret) << "\n";
            return false;
        }
    }

    // set format-level options (e.g., rtsp_transport)
    AVDictionary *fmt_opts = nullptr;
    av_dict_set(&fmt_opts, "rtsp_transport", "tcp", 0); // use TCP for RTSP by default
    // write header
    int hdr_ret = avformat_write_header(ofmt_ctx_, &fmt_opts);
    if (hdr_ret < 0) {
        std::cerr << "Error occurred when writing header: " << ffErr2Str(hdr_ret) << "\n";
        av_dict_free(&fmt_opts);
        return false;
    }
    av_dict_free(&fmt_opts);

    // prepare sws if input pixfmt != encoder pixfmt
    if (in_pix_fmt_ != enc_pix_fmt_) {
        sws_ctx_ = sws_getContext(width_, height_, in_pix_fmt_,
                                  width_, height_, enc_pix_fmt_,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            std::cerr << "Could not create sws context\n";
            return false;
        }
        std::cerr << "[RTSP] Color conversion enabled via sws_scale\n";
    } else {
        std::cerr << "[RTSP] Color conversion disabled (direct input to encoder)\n";
    }

    // start encoding thread
    running_ = true;
    encoder_thread_ = std::thread(&FFmpegStreamer::encodingThreadFunc, this);

    std::cerr << "FFmpegStreamer opened RTSP -> " << url_ << "\n";
    return true;
}

void FFmpegStreamer::pushFrame(std::vector<uint8_t> &&frame) {
    if (!running_) return;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (frame_queue_.size() >= max_queue_size_) {
            frame_queue_.pop(); // drop oldest
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        frame_queue_.push(std::move(frame));
    }
    cv_.notify_one();
}

void FFmpegStreamer::encodingThreadFunc() {
    // prepare reusable objects
    AVFrame *frame = av_frame_alloc();
    frame->format = enc_pix_fmt_;
    frame->width = width_;
    frame->height = height_;
    int ret = av_image_alloc(frame->data, frame->linesize, width_, height_, enc_pix_fmt_, 1);
    if (ret < 0) {
        std::cerr << "Could not alloc image buffers\n";
        av_frame_free(&frame);
        return;
    }

    AVPacket *pkt = av_packet_alloc();

    while (running_) {
        std::vector<uint8_t> cur;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]{ return !frame_queue_.empty() || !running_; });
            if (!running_ && frame_queue_.empty()) break;
            cur = std::move(frame_queue_.front());
            frame_queue_.pop();
        }
        // Build AVFrame from cur data (cur contains concatenated planes)
        // Need to handle both input pixfmt == enc_pix_fmt or conversion via sws_scale

        // If input pixfmt == enc_pix_fmt_ and memory layout matches, we can fill directly.
        if (in_pix_fmt_ == enc_pix_fmt_) {
            // Fill frame->data using av_image_fill_arrays from cur.data()
            av_image_fill_arrays(frame->data, frame->linesize, cur.data(), enc_pix_fmt_, width_, height_, 1);
        } else {
            // Prepare src data pointers & linesizes for sws_scale.
            // We must reconstruct src pointers from concatenated planes. For common formats:
            // - NV12: plane0(Y) size = width*height, plane1(UV interleaved) size = width*height/2
            // - YUV420P: plane sizes Y,width*height; U,width*height/4; V,width*height/4
            // We'll build src_data and src_linesize accordingly.
            uint8_t *src_data[4] = {nullptr};
            int src_linesize[4] = {0};
            if (in_pix_fmt_ == AV_PIX_FMT_NV12) {
                size_t y_size = width_ * height_;
                size_t uv_size = (width_ * height_) / 2;
                if (cur.size() < y_size + uv_size) {
                    std::cerr << "Frame size too small\n";
                    continue;
                }
                src_data[0] = const_cast<uint8_t*>(cur.data());
                src_data[1] = const_cast<uint8_t*>(cur.data() + y_size);
                src_linesize[0] = width_;
                src_linesize[1] = width_;
            } else if (in_pix_fmt_ == AV_PIX_FMT_YUV420P) {
                size_t y_size = width_ * height_;
                size_t u_size = (width_/2) * (height_/2);
                size_t v_size = u_size;
                if (cur.size() < y_size + u_size + v_size) {
                    std::cerr << "Frame size too small\n";
                    continue;
                }
                src_data[0] = const_cast<uint8_t*>(cur.data());
                src_data[1] = const_cast<uint8_t*>(cur.data() + y_size);
                src_data[2] = const_cast<uint8_t*>(cur.data() + y_size + u_size);
                src_linesize[0] = width_;
                src_linesize[1] = width_/2;
                src_linesize[2] = width_/2;
            } else {
                std::cerr << "Unsupported input pixfmt for conversion\n";
                continue;
            }

            // Convert to enc_pix_fmt_ (YUV420P)
            sws_scale(sws_ctx_, src_data, src_linesize, 0, height_, frame->data, frame->linesize);
        }

        // set pts
        frame->pts = pts_counter_++;

        // send to encoder
        ret = avcodec_send_frame(enc_ctx_, frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder: " << ret << "\n";
            continue;
        }
        encoded_frames_.fetch_add(1, std::memory_order_relaxed);

        // receive packets
        while (ret >= 0) {
            ret = avcodec_receive_packet(enc_ctx_, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                std::cerr << "Error during encoding\n";
                break;
            }
            // Rescale packet pts/dts
            av_packet_rescale_ts(pkt, enc_ctx_->time_base, out_stream_->time_base);
            pkt->stream_index = out_stream_->index;
            // write packet
            ret = av_interleaved_write_frame(ofmt_ctx_, pkt);
            if (ret < 0) {
                std::cerr << "Error writing packet: " << ret << "\n";
            } else {
                sent_packets_.fetch_add(1, std::memory_order_relaxed);
            }
            av_packet_unref(pkt);
        }
    }

    // flush encoder
    avcodec_send_frame(enc_ctx_, nullptr);
    while (avcodec_receive_packet(enc_ctx_, pkt) == 0) {
        av_packet_rescale_ts(pkt, enc_ctx_->time_base, out_stream_->time_base);
        pkt->stream_index = out_stream_->index;
        av_interleaved_write_frame(ofmt_ctx_, pkt);
        av_packet_unref(pkt);
    }

    // free
    av_freep(&frame->data[0]);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void FFmpegStreamer::close() {
    if (!running_) {
        // still may need to cleanup if contexts exist
        cleanup();
        return;
    }
    running_ = false;
    cv_.notify_one();
    if (encoder_thread_.joinable()) encoder_thread_.join();
    cleanup();
}

void FFmpegStreamer::cleanup() {
    if (ofmt_ctx_) {
        av_write_trailer(ofmt_ctx_);
        if (!(ofmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx_->pb);
        }
        avformat_free_context(ofmt_ctx_);
        ofmt_ctx_ = nullptr;
    }
    if (enc_ctx_) {
        avcodec_free_context(&enc_ctx_);
        enc_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    // clear queue
    std::lock_guard<std::mutex> lk(mtx_);
    while (!frame_queue_.empty()) frame_queue_.pop();
}
