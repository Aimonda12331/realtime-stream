#include "camera.h"

// FFmpeg streamer (optional): used when a RTSP URL is set via setRtsp()
#include "streamer.h"

#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <type_traits>
#include <utility>

#include <libcamera/control_ids.h>

using namespace libcamera;
using namespace std::chrono_literals; 

namespace {
template <typename T, typename = void>
struct HasTransformMember : std::false_type {};

template <typename T>
struct HasTransformMember<T, std::void_t<decltype(std::declval<T &>().transform)>> : std::true_type {};

template <typename T>
bool applyTransformIfSupported(T &cfg, const Transform &t) {
    if constexpr (HasTransformMember<T>::value) {
        cfg.transform = t;
        return true;
    }
    (void)cfg;
    (void)t;
    return false;
}
}

// ════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ════════════════════════════════════════════════════════════════

CameraStreamer::CameraStreamer() = default;

CameraStreamer::~CameraStreamer() { release(); }

// ════════════════════════════════════════════════════════════════
// init — khởi tạo CameraManager, chọn & acquire camera đầu tiên
// ════════════════════════════════════════════════════════════════

bool CameraStreamer::init () {
    cm_.start();
    cmStarted_ = true;

    if (cm_.cameras().empty()) {
        std::cerr << "Khong tim thay camera\n";
        release();
        return false;
    }
    std::cerr << "Co " << cm_.cameras().size() << " camera kha dung\n";

    camera_ = cm_.cameras()[0];
    if (!camera_) {
        std::cerr << "Khong the lay camera\n";
        release();
        return false;
    }

    if (camera_->acquire() < 0) {
        std::cerr << "Khong the chiem duoc camera\n";
        camera_.reset();
        release();
        return false;
    }
    return true;
}

int CameraStreamer::configure(PixelFormat fmt, Size size) {
    auto config = camera_->generateConfiguration({ StreamRole::VideoRecording });
    if (!config) {
        std::cerr << "generateConfiguration() that bai\n";
        return -1;
    }

    if (applyPixelFormat(config.get(), fmt, size) < 0) 
        return -1;

    Transform t = Transform::Identity;
    if (hflip_ && vflip_) t = Transform::Rot180;
    else if (hflip_) t = Transform::HFlip;
    else if (vflip_) t = Transform::VFlip;

    const bool transformApplied = applyTransformIfSupported(*config, t);
    std::cerr << "[config] transform="
              << (hflip_ ? "H" : "-")
              << (vflip_ ? "V" : "-")
              << (transformApplied ? "" : " (not supported by this libcamera build)")
              << "\n";

    if (camera_->configure(config.get()) < 0) {
        std::cerr << "camera->configure() that bai\n";
        return -1;
    }

    StreamConfiguration &applied = config->at(0);
    std::cerr << "Configured: " << applied.pixelFormat.toString()
              << " " << applied.size.width << "x" << applied.size.height << "\n";

    stream_ = applied.stream();
    configured_pixfmt_ = applied.pixelFormat;
    configured_size_ = applied.size;
    return 0;
}

// ════════════════════════════════════════════════════════════════
// applyPixelFormat — thử YUV420 → fallback NV12
// ════════════════════════════════════════════════════════════════

int CameraStreamer::applyPixelFormat(CameraConfiguration *config,
                                     PixelFormat fmt, Size size) {
    if (!config) return -1;
    StreamConfiguration &sc = config->at(0);

    std::cerr << "[config] Format mac dinh: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";

    sc.pixelFormat = fmt;
    sc.size        = size;
    // NOTE: FPS được khóa qua FrameDurationLimits control khi camera_->start(),
    // KHÔNG dùng sc.frameRate (không tồn tại trong libcamera StreamConfiguration).

    auto st = config->validate();

    if (st == CameraConfiguration::Invalid) {
        std::cerr << "Config khong hop le voi " << fmt.toString() << ", thu NV12\n";
        sc.pixelFormat = formats::NV12;
        st = config->validate();
    } else if (st == CameraConfiguration::Adjusted) {
        std::cerr << "Config duoc dieu chinh (khong chap nhan)\n";
        return -1;
    }

    if (st == CameraConfiguration::Adjusted) {
        std::cerr << "[config] Adjusted -> "
                  << sc.pixelFormat.toString()
                  << " " << sc.size.width << "x" << sc.size.height << "\n";
    }

    std::cerr << "[config] Format cuoi: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════
// startStreaming — allocate buffers, pre-mmap, worker thread, loop
// ════════════════════════════════════════════════════════════════

int CameraStreamer::startStreaming() {
    if (!stream_) return -1;

    //Bước 1: Cấp phát buffer
    FrameBufferAllocator allocator(camera_);
    if (allocator.allocate(stream_) < 0) {
        std::cerr << "allocator.allocate() that bai\n";
        return -1;
    }

    const auto &buffers = allocator.buffers(stream_);
    std::cerr << "\nSo buffer duoc cap phat: " << buffers.size() << "\n";
    printPlaneInfo(buffers);

    // Bước 2: Pre-mmap
    premapBuffers(buffers);

    // Pre-allocate buffer pool
    {
        size_t frame_size = configured_size_.width * configured_size_.height * 3 / 2;
        std::lock_guard<std::mutex> lk(pool_mutex_);
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            buffer_pool_.push(std::vector<uint8_t>(frame_size));
        }
    }

    //Bước 3: Tạo Request cho mỗi buffer
    std::vector<std::unique_ptr<Request>> requests;
    for (const auto &buf : buffers) {
        std::unique_ptr<Request> req(camera_->createRequest());
        if(!req) {
            std::cerr << "createRequest() that bai\n";
            cleanupMmaps();
            allocator.free(stream_);
            return -1; 
        }
        if(req->addBuffer(stream_, buf.get()) < 0) {
            std::cerr << "addBuffer() that bai\n";
            cleanupMmaps();
            allocator.free(stream_);
            return -1;
        }
        requests.push_back(std::move(req));
    }

    // If RTSP URL was set, open FFmpeg streamer
    if (!rtsp_url_.empty()) {
        ff_ = std::make_unique<FFmpegStreamer>();
        AVPixelFormat in_pixfmt = AV_PIX_FMT_NV12;
        if (configured_pixfmt_ == libcamera::formats::NV12) in_pixfmt = AV_PIX_FMT_NV12;
        else if (configured_pixfmt_ == libcamera::formats::YUV420) in_pixfmt = AV_PIX_FMT_YUV420P;
        else {
            std::cerr << "[RTSP] Unrecognized pixel format, falling back to NV12\n";
            in_pixfmt = AV_PIX_FMT_NV12;
        }
        if (!ff_->open(rtsp_url_, configured_size_.width, configured_size_.height, in_pixfmt, out_fps_)) {
            std::cerr << "[RTSP] Cannot open FFmpegStreamer for URL: " << rtsp_url_ << "\n";
            cleanupMmaps();
            allocator.free(stream_);
            return -1;
        }
        std::cerr << "[RTSP] Output sink: " << rtsp_url_ << "\n";
    } else {
        std::cerr << "[RTSP] No URL configured, output sink: stdout raw video\n";
    }

    //Bước 4: Worker thread
    running_        = true;
    workerAlive_    = true;
    frameCount_     = 0;

    std::thread worker([this]() {
        while(workerAlive_.load() || !jobQueue_.empty()) {
            FrameJob job;
            {
                std::unique_lock<std::mutex> lk(queueMutex_);
                queueCv_.wait(lk, [this]{
                    return !jobQueue_.empty() || !workerAlive_.load();
                });        
                if (jobQueue_.empty()) continue;
                job = std::move(jobQueue_.front());
                jobQueue_.pop();
            }
            if (ff_) {
                // Move frame ownership into FFmpeg queue to avoid one extra copy.
                // Do NOT release buffer here because ownership transferred to ff_.
                ff_->pushFrame(std::move(job.data));
            } else {
                if(!writeAll(STDOUT_FILENO, job.data.data(), job.data.size())) {
                    std::cerr << "write stdout that bai, dung stream\n";
                    running_ = false;
                    break;
                }
                // Return buffer to pool when we consumed it locally
                releaseBuffer(std::move(job.data));
            }
        }
    });

    //Bước 5: kết nối callback
    camera_->requestCompleted.connect(this, &CameraStreamer::onRequestComplete);

    //Bước 6: Start camera VỚI FrameDurationLimits để KHÓA FPS sensor
    ControlList controls;
    if (target_fps_ > 0) {
        int64_t frame_duration_us = 1000000 / target_fps_;
        // Span<const int64_t, 2> yêu cầu mảng 2 phần tử [min, max]
        int64_t limits[2] = { frame_duration_us, frame_duration_us };
        controls.set(libcamera::controls::FrameDurationLimits,
                     libcamera::Span<const int64_t, 2>(limits));
        std::cerr << "[FPS] Khoa camera sensor tai " << target_fps_
                  << " fps (duration=" << frame_duration_us << " us)\n";
    }

    if (camera_->start(&controls) < 0) {
        std::cerr << "camera->start() that bai\n";
        camera_->requestCompleted.disconnect(this);
        workerAlive_ = false;
        queueCv_.notify_one();
        if (worker.joinable()) worker.join();
        cleanupMmaps();
        allocator.free(stream_);
        return -1;
    }
    cameraStarted_ = true;
    std::cerr << "Streaming started — pipe stdout to ffmpeg/ffplay. Ctrl+C to stop.\n";

    for (auto &r : requests) {
        if (camera_->queueRequest(r.get()) < 0)
            std::cerr << "queueRequest() that bai\n";
    }

    //Bước 7: FPS monitoring loop
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t lastCount = 0;
    uint64_t lastEncCount = 0;
    uint64_t lastSentCount = 0;

    while (running_.load()) {
        std::this_thread::sleep_for(100ms);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
        if (elapsed >= 3000) {
            uint64_t cur = frameCount_.load(std::memory_order_relaxed);
            uint64_t dropped = dropped_jobs_.load(std::memory_order_relaxed);
            double fps = (cur - lastCount) * 1000.0 / elapsed;

            std::cerr << "[Streaming] frames=" << cur
                      << " dropped=" << dropped
                      << " capture_fps=" << std::fixed << std::setprecision(1) << fps;

            if (ff_) {
                uint64_t enc = ff_->encodedFrames();
                uint64_t sent = ff_->sentPackets();
                double enc_fps = (enc - lastEncCount) * 1000.0 / elapsed;
                double send_fps = (sent - lastSentCount) * 1000.0 / elapsed;
                std::cerr << " | enc_fps=" << std::setprecision(1) << enc_fps
                          << " send_fps=" << std::setprecision(1) << send_fps;
                lastEncCount = enc;
                lastSentCount = sent;
            }
            std::cerr << "\n";

            lastCount = cur;
            lastReport = now;
        }
    }

    // ---- shutdown ----
    camera_->requestCompleted.disconnect(this);
    camera_->stop();
    cameraStarted_ = false;

    workerAlive_ = false;
    queueCv_.notify_one();
    if (worker.joinable()) worker.join();

    cleanupMmaps();
    allocator.free(stream_);

    std::cerr << "[streaming] Tong frames: " << frameCount_.load() << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════
// stop
// ════════════════════════════════════════════════════════════════

void CameraStreamer::stop() {
    running_ = false;
}

// ════════════════════════════════════════════════════════════════
// release
// ════════════════════════════════════════════════════════════════

void CameraStreamer::release() {
    running_ = false;

    if (ff_) {
        ff_->close();
        ff_.reset();
    }

    if(camera_) {
        if (cameraStarted_) {
            camera_->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cameraStarted_ = false;
        }
        stream_ = nullptr;
        camera_->release();
        camera_.reset();
    }

    if(cmStarted_) {
        cm_.stop();
        cmStarted_ = false;
    }

    std::cerr << "camera va CameraManager da duoc giai phong\n";
}

// ════════════════════════════════════════════════════════════════
// onRequestComplete — drop-before-copy, buffer pool, requeue
// ════════════════════════════════════════════════════════════════

void CameraStreamer::onRequestComplete(Request *request) {
    if (!running_.load()) return;

    const auto &bufferMap = request->buffers();
    auto it = bufferMap.find(stream_);
    if (it == bufferMap.end()) return;

    // DROP TRƯỚC KHI COPY: nếu queue đầy, bỏ frame này (tiết kiệm CPU)
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (jobQueue_.size() >= max_job_queue_size_) {
            dropped_jobs_.fetch_add(1, std::memory_order_relaxed);
            request->reuse(Request::ReuseBuffers);
            if (camera_) camera_->queueRequest(request);
            frameCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    FrameBuffer *fb = it->second;
    const auto &planes = fb->planes();
    if (planes.empty()) {
        request->reuse(Request::ReuseBuffers);
        if (camera_) camera_->queueRequest(request);
        return;
    }

    size_t totalLen = 0;
    for (const auto &pl : planes)
        totalLen += pl.length;

    // Copy từ pre-mapped memory dùng buffer pool
    FrameJob job;
    job.data = acquireBuffer(totalLen);
    size_t pos = 0;
    for (const auto &pl : planes) {
        int fd = pl.fd.get();
        auto mit = mmaps_.find(fd);
        if (mit == mmaps_.end()) continue;
        std::memcpy(job.data.data() + pos, mit->second.base + pl.offset, pl.length);
        pos += pl.length;
    }

    // Requeue ngay
    request->reuse(Request::ReuseBuffers);
    if (camera_ && camera_->queueRequest(request) < 0)
        std::cerr << "queueRequest() that bai khi re-queue\n";

    // Push job
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        jobQueue_.push(std::move(job));
    }
    queueCv_.notify_one();
    frameCount_.fetch_add(1, std::memory_order_relaxed);
}

// ════════════════════════════════════════════════════════════════
// premapBuffers
// ════════════════════════════════════════════════════════════════

void CameraStreamer::premapBuffers(const std::vector<std::unique_ptr<FrameBuffer>> &buffers) {
    for (const auto &buf : buffers) {
            for(const auto &pl : buf->planes()) {
                int fd = pl.fd.get();
                size_t end = static_cast<size_t>(pl.offset) + pl.length;
                if (mmaps_.count(fd)) {
                    if (end > mmaps_[fd].size) mmaps_[fd].size = end;
                    continue;
                }
                mmaps_[fd] = {nullptr, end};
        }
    }
    
    for (auto &[fd, entry] : mmaps_) {
        void *base = mmap(nullptr, entry.size, PROT_READ, MAP_SHARED, fd, 0);
        if(base == MAP_FAILED) {
            std::cerr <<  "pre-mmap(fd=" << fd << ") failed: " << strerror(errno) << "\n";
            entry.base = nullptr;
        } else {
            entry.base = static_cast<uint8_t *> (base);
            std::cerr << "pre-mmap fd=" << fd << " size=" << entry.size << " OK\n";
        }
    }
}

// ════════════════════════════════════════════════════════════════
// cleanupMmaps
// ════════════════════════════════════════════════════════════════

void CameraStreamer::cleanupMmaps() {
    for (auto &[fd, entry] : mmaps_) {
        if (entry.base)
        munmap(entry.base, entry.size);
    }
    mmaps_.clear();
}

// ════════════════════════════════════════════════════════════════
// printPlanesInfo
// ════════════════════════════════════════════════════════════════

void CameraStreamer::printPlaneInfo(const std::vector<std::unique_ptr<FrameBuffer>> &buffers) {
    for (size_t i = 0; i < buffers.size(); ++i) {
        const auto &planes = buffers[i]->planes();
        std::cerr << "buffer[" << i << "] planes=" << planes.size() << "\n";

        for (size_t p = 0; p < planes.size(); ++p) {
            const auto &pl = planes[p];
            std::cerr << " plane " << p
                      << " fd=" << pl.fd.get()
                      << " offset=" << pl.offset
                      << " length=" << pl.length << "\n";
        }
    }
}

// ════════════════════════════════════════════════════════════════
// writeAll
// ════════════════════════════════════════════════════════════════

bool CameraStreamer::writeAll(int fd, const void *buf, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t remain = size;
    while (remain >0) {
        ssize_t w = ::write(fd, p, remain);
        if ( w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        remain -= static_cast<size_t>(w);
        p += w;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════
// Buffer pool
// ════════════════════════════════════════════════════════════════

std::vector<uint8_t> CameraStreamer::acquireBuffer(size_t size) {
    std::lock_guard<std::mutex> lk(pool_mutex_);
    if (!buffer_pool_.empty()) {
        auto buf = std::move(buffer_pool_.front());
        buffer_pool_.pop();
        if (buf.capacity() >= size) {
            buf.resize(size);
            return buf;
        }
    }
    return std::vector<uint8_t>(size);
}

void CameraStreamer::releaseBuffer(std::vector<uint8_t> &&buf) {
    std::lock_guard<std::mutex> lk(pool_mutex_);
    if (buffer_pool_.size() < POOL_SIZE) {
        buffer_pool_.push(std::move(buf));
    }
}
