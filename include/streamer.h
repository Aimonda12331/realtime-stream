// FFmpegStreamer.h
#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>

class FFmpegStreamer {
public:
    FFmpegStreamer();
    ~FFmpegStreamer();

    // Open RTSP output. in_pixfmt is AVPixelFormat of raw frames coming from camera (e.g., AV_PIX_FMT_YUV420P or AV_PIX_FMT_NV12)
    bool open(const std::string &url, int width, int height, AVPixelFormat in_pixfmt, int fps);
    // Push one raw frame (concatenated planes as your current job.data contains)
    void pushFrame(std::vector<uint8_t> &&frame);
    // Stop and cleanup (blocking)
    void close();

private:
    void encodingThreadFunc();
    bool initEncoder();
    void cleanup();

    // ffmpeg contexts
    AVFormatContext *ofmt_ctx_{nullptr};
    AVCodecContext *enc_ctx_{nullptr};
    AVStream *out_stream_{nullptr};
    SwsContext *sws_ctx_{nullptr};
    size_t max_queue_size_{2};

    // input params
    int width_{0}, height_{0}, fps_{0};
    AVPixelFormat in_pix_fmt_{AV_PIX_FMT_NONE};
    AVPixelFormat enc_pix_fmt_{AV_PIX_FMT_YUV420P}; // encoder uses yuv420p

    std::thread encoder_thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::vector<uint8_t>> frame_queue_;
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> encoded_frames_{0};
    std::atomic<uint64_t> sent_packets_{0};
    std::atomic<bool> running_{false};
    // pts_counter_ đã bị thay thế bởi wall-clock PTS trong encodingThreadFunc

    // options
    std::string url_;

public:
    uint64_t encodedFrames() const { return encoded_frames_.load(std::memory_order_relaxed); }
    uint64_t sentPackets() const { return sent_packets_.load(std::memory_order_relaxed); }
};
