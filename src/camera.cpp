#include "camera.h"

#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

using namespace libcamera;
using namespace std::chrono_literals; 

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
    std::cerr << "Co " << cm_.cameras().size() << "camera kha dung \n";

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

    if (camera_->configure(config.get()) < 0) {
        std::cerr << "camera->configure() that bai\n";
        return -1;
    }

    StreamConfiguration &applied = config->at(0);
    std::cerr << "Configured: " << applied.pixelFormat.toString()
              << " " << applied.size.width << "x" << applied.size.height << "\n";

    stream_ = applied.stream();
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

    //Tạo bộ nhớ đếm (buffer) để chứa dữ liệu ảnh từ camera. In ra số buffer được cấp phát
    const auto &buffers = allocator.buffers(stream_);
    std::cerr << "\nSo buffer duoc cap phat: " << buffers.size() << "\n";
    printPlaneInfo(buffers);

    // Bước 2: Pre-mmap
    premapBuffers(buffers); 

    //Bước 3: Tạo Request cho  mỗi buffer
    std::vector<std::unique_ptr<Request>> requests;
    for (const auto &buf : buffers) {
        std::unique_ptr<Request> req(camera_->createRequest()); //Tạo request cho mỗi buffer
        if(!req) {
            std::cerr << "createRequest() that bai\n";
            cleanupMmaps();
            allocator.free(stream_);
            return -1; 
        }
        if(req->addBuffer(stream_, buf.get()) < 0) {//Tạo Request cho mỗi buffer
            std::cerr << "addBuffer() that bai\n";
            cleanupMmaps();
            allocator.free(stream_);
            return -1;
        }
        requests.push_back(std::move(req));
    }

    // Bước 4: Worker thread - pop job -> write stdout
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
                if (jobQueue_.empty()) {
                    continue;
                }
                job = std::move(jobQueue_.front());
                jobQueue_.pop();
            }
            if(!writeAll(STDOUT_FILENO, job.data.data(), job.data.size())) {
                std::cerr << "write stdout that bai, dung stream\n";
                running_ = false;
                break;
            }
        }
    });

    //Bước 5: kết nối callback
    camera_->requestCompleted.connect(this, &CameraStreamer::onRequestComplete);

    //Bước 6: Start camera
    if (camera_->start() < 0) {
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

    //Bước 7: FPS monitoring loop - chờ đến khi stop()
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t lastCount = 0;
    while (running_.load()) {
        std::this_thread::sleep_for(100ms);
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
        if (elapsed >= 3000) {
            uint64_t cur = frameCount_.load(std::memory_order_relaxed);
            double fps   =(cur - lastCount) * 1000.0 / elapsed;
            std::cerr << "[Streaming] frames=" << cur
                      << " fps=" << std::fixed << std::setprecision(1) << fps << "\n";
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
// stop — đặt cờ dừng (thread-safe, gọi từ signal handler)
// ════════════════════════════════════════════════════════════════

void CameraStreamer::stop() {
    running_ = false;
}

// ════════════════════════════════════════════════════════════════
// release — giải phóng toàn bộ tài nguyên
// ════════════════════════════════════════════════════════════════

void CameraStreamer::release() {
    running_ = false;

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
// onRequestComplete — callback: copy frame → requeue → push job
// ════════════════════════════════════════════════════════════════

void CameraStreamer::onRequestComplete(Request *request) {
    if (!running_.load()) return; //kiểm tra trang thai

    //Lấy buffer từ request
    //Mỗi request có thể chứa nhiều buffer. Ở đây tìm buffer gắn với stream_ và lấy ra
    const auto &bufferMap = request->buffers();
    auto it = bufferMap.find(stream_);
    if (it == bufferMap.end()) return;

    FrameBuffer *fb = it->second;

    //Lấy thông tin cá plane
    /// Một frame có thể chia thành nhiều plane (ví dụ YUV có Y,U,V). Nếu không có plane thì bỏ qua
    const auto &planes = fb->planes();
    if (planes.empty()) return;

    // Tổng kích thước frame
    /// Cộng tất cả các độ dài của các plane để biết tổng dung lượng frame
    size_t totalLen = 0;
    for (const auto &pl : planes)
        totalLen += pl.length;

    // Copy từ pre-mapped memory
    /// Dùng memcpy để copy dữ liệu từ vùng nhớ đã mmap sẵn (mmaps_) vào job.dât. Mỗi plane được copy nối tiếp nhau
    FrameJob job;
    job.data.resize(totalLen);
    size_t pos = 0;
    for (const auto &pl : planes) {
        int fd = pl.fd.get();
        auto mit = mmaps_.find(fd);
        if (mit == mmaps_.end()) continue;
        std::memcpy(job.data.data() + pos, mit->second.base + pl.offset, pl.length);
        pos += pl.length;
    }

    // Requeue ngay (buffer đã copy xong)
    /// Sau khi copy xong, buffer được tái sử dụng ngay cho lần chụp tiếp theo. Điều này giúp camera chạy liên tục mà không cần cấp phát lại
    request->reuse(Request::ReuseBuffers);
    if (camera_ && camera_->queueRequest(request) < 0)
        std::cerr << "queueRequest() that bai khi re-queue\n";

    // Push job: đưa job vào hàng đợi
    /*Frame vừa coppy được đóng gói thành FrameJob và đưa vào jobQueue_. 
    Worker thread sẽ lấy job này để ghi ra stdout. Đồng thời tăng bộ đếm frame*/ 
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        jobQueue_.push(std::move(job));
    }
    queueCv_.notify_one();
    frameCount_.fetch_add(1, std::memory_order_relaxed);
}

// ════════════════════════════════════════════════════════════════
// premapBuffers — mmap tất cả dmabuf fd 1 lần
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
// writeAll — ghi đầy đủ bytes ra fd
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