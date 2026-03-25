// g++ -std=c++17 -O2 6_capture_streaming.cpp -o 6_capture_streaming `pkg-config --cflags --libs libcamera` -pthread -latomic
#include <iostream>
#include <iomanip>
#include <thread>
#include <memory>
#include <chrono>
#include <unistd.h>
#include <stdlib.h>
#include <atomic>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <cstring>
#include <cerrno>

#include <sys/mman.h>
#include <signal.h>

#include <libcamera/libcamera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/formats.h>

using namespace libcamera;
using namespace std::chrono_literals;

static CameraManager cm;   //cm là biến đại diện cho đối tượng CameraManager
static std::shared_ptr<Camera> camera; //Khai báo biến toàn cục để sau này callback có thể truy cập
static Stream *g_stream = nullptr;

//Biến toàn cục để dùng capture bằng Ctr-C
static volatile bool g_running = true;
static void sigint_handler(int) { g_running = false; }

static bool cm_started = false; // cờ bảo vệ việc giải phóng (hàm release) // CameraManager đã start
static bool camera_started = false; // Camera đã start (đang streaming)

bool initCameraManager ();
int  cameraConfigure ();
int  pixelFormating (CameraConfiguration *config); //khai bao config trong prototype
int  getAllocateBuffer ();
void printPlanesInfor(const std::vector<std::unique_ptr<FrameBuffer>> &buffers);
bool releaseCameraManager ();

//start
bool initCameraManager () {
    cm.start();
    cm_started = true;

    if(cm.cameras().empty()) {
        std::cerr << "Không tìm thấy camera \n" << std::endl;
        releaseCameraManager();
        return false; //thất bại
    }
    std::cerr << "Co " << cm.cameras().size() << " camera kha dung \n" << std::endl;

    //Chọn camera đầu tiên
    camera = cm.cameras()[0];
    if (!camera) {
        std::cerr << "Khong the lay camera\n";
        releaseCameraManager();
        return false;
    }

    if (camera->acquire() < 0) {
        std::cerr << "Khong the chiem duoc camera hic\n" ;
        camera.reset(); //reset camera trước khi release
        releaseCameraManager();
        return false;
    }

    return true;
}

int cameraConfigure() {
    //Tạo config cho VideoRecording 
    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::VideoRecording });
    if(!config) {
        std::cerr << "generateConfiguration() that bai\n";
        releaseCameraManager();
        return -1;
    }

    //truyen config vao ham format/validate
    int p = pixelFormating(config.get());
    if (p < 0){
        releaseCameraManager();
        return p;
    }

    // Áp dụng cấu hình
    if (camera->configure(config.get()) < 0) {
        std::cerr << "cam->configure() thất bại\n";
        releaseCameraManager();
        return -1;
    }

    // In pixel format và kích thước đã áp dụng (ra stderr, vì stdout dùng cho raw data)
    StreamConfiguration &applied = config->at(0);
    std::cerr << "Configured: " << applied.pixelFormat.toString()
              << " " << applied.size.width << "x" << applied.size.height << "\n";

    g_stream = config->at(0).stream(); //Thêm biến toàn cục g_stream để lưu Stream* sau khi configure xong
    return 0;
}

// - Valid → cấu hình hợp lệ, có thể dùng.
// - Adjusted → cấu hình đã được chỉnh sửa để phù hợp với phần cứng (ví dụ bạn yêu cầu 1920x1080 nhưng camera chỉ hỗ trợ 1280x720, nó sẽ tự điều chỉnh).
// - Invalid → cấu hình không hợp lệ, không thể dùng để khởi động camera.

int pixelFormating(CameraConfiguration *config) {
    // Thu ep theo YUV420 va kich thuoc 640x480
     if (!config) return -1;
    StreamConfiguration &sc = config->at(0);

    //Log format mặc định camera de xuat
    std::cerr << "[pixelFormating] Format mặc định: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";

    sc.pixelFormat = formats::YUV420;
    sc.size = Size(640, 480);

    CameraConfiguration::Status st = config->validate();
     
    if(st == CameraConfiguration::Invalid) { //Check YUV ko dc check NV12
        std::cerr << "Cau hinh khong hop le voi YUV420, thu NV12\n";
        sc.pixelFormat = formats::NV12;
        st = config->validate();
    } else if( st == CameraConfiguration::Adjusted) {
        std::cerr << "config duoc dieu chinh\n";
        return -1;
    }

    if (st == CameraConfiguration::Adjusted) {
        std::cerr << "[pixelFormating] Config được điều chỉnh → "
                  << sc.pixelFormat.toString()
                  << " " << sc.size.width << "x" << sc.size.height << "\n";
    }

    // Log format cuối cùng được áp dụng
    std::cerr << "[pixelFormating] Format cuối: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";
    return 0;
}

// ── Job chứa bản copy dữ liệu frame (an toàn — không phụ thuộc dmabuf nữa) ──
struct FrameJob {
    std::vector<uint8_t> data;  // Y+U+V đã copy
};

// ── Helper: ghi đầy đủ bytes ra fd (vòng lặp write) ──
static bool write_all(int fd, const void *buf, size_t size) {
    const uint8_t *p = static_cast<const uint8_t*>(buf);
    size_t remain = size;
    while (remain > 0) {
        ssize_t w = ::write(fd, p, remain);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        remain -= static_cast<size_t>(w);
        p += w;
    }
    return true;
}

// ── Bảng pre-mmap: fd → {base_ptr, mapped_size} ──
struct MmapEntry {
    uint8_t *base = nullptr;
    size_t   size = 0;
};
static std::map<int, MmapEntry> g_mmaps;  // pre-mmap tất cả buffer fd

// ── Handler: copy dữ liệu frame từ pre-mapped memory, requeue, push job ──
struct CaptureHandler {
    Stream              *stream         = nullptr;
    Camera              *cam            = nullptr;
    std::queue<FrameJob>        *q      = nullptr;
    std::mutex                  *qmutex = nullptr;
    std::condition_variable     *qcv    = nullptr;
    std::atomic<uint64_t>       *frameCount = nullptr;

    void onRequestComplete(Request *request) {
        if (!g_running) return;

        const auto &bufferMap = request->buffers();
        auto it = bufferMap.find(stream);
        if (it == bufferMap.end()) return;
        FrameBuffer *fb = it->second;
        const auto &planes = fb->planes();
        if (planes.empty()) return;

        // Tính tổng kích thước frame
        size_t totalLen = 0;
        for (const auto &pl : planes)
            totalLen += pl.length;

        // Copy dữ liệu từ pre-mapped memory vào vector (memcpy nhanh, ~0.5ms cho 460KB)
        FrameJob job;
        job.data.resize(totalLen);
        size_t pos = 0;
        for (const auto &pl : planes) {
            int fd = pl.fd.get();
            auto mit = g_mmaps.find(fd);
            if (mit == g_mmaps.end()) continue;
            std::memcpy(job.data.data() + pos, mit->second.base + pl.offset, pl.length);
            pos += pl.length;
        }

        // Requeue request ngay (buffer đã được copy, an toàn)
        request->reuse(Request::ReuseBuffers);
        if (cam && cam->queueRequest(request) < 0)
            std::cerr << "queueRequest() that bai khi re-queue\n";

        // Push job vào queue
        {
            std::lock_guard<std::mutex> lk(*qmutex);
            q->push(std::move(job));
        }
        qcv->notify_one();

        if (frameCount) frameCount->fetch_add(1, std::memory_order_relaxed);
    }
};

// ── Pre-mmap tất cả dmabuf fd của các buffer (gọi 1 lần sau allocate) ──
static void premmap_buffers(const std::vector<std::unique_ptr<FrameBuffer>> &buffers) {
    for (const auto &buf : buffers) {
        for (const auto &pl : buf->planes()) {
            int fd = pl.fd.get();
            if (g_mmaps.count(fd)) {
                // Đã mmap fd này, chỉ cần mở rộng nếu cần
                size_t end = static_cast<size_t>(pl.offset) + pl.length;
                if (end > g_mmaps[fd].size) g_mmaps[fd].size = end;
                continue;
            }
            size_t end = static_cast<size_t>(pl.offset) + pl.length;
            g_mmaps[fd] = {nullptr, end};
        }
    }
    for (auto &pair : g_mmaps) {
        void *base = mmap(nullptr, pair.second.size, PROT_READ, MAP_SHARED, pair.first, 0);
        if (base == MAP_FAILED) {
            std::cerr << "pre-mmap(fd=" << pair.first << ") failed: " << strerror(errno) << "\n";
            pair.second.base = nullptr;
        } else {
            pair.second.base = static_cast<uint8_t*>(base);
            std::cerr << "pre-mmap fd=" << pair.first << " size=" << pair.second.size << " OK\n";
        }
    }
}

// ── Giải phóng tất cả pre-mmap ──
static void cleanup_mmaps() {
    for (auto &pair : g_mmaps) {
        if (pair.second.base)
            munmap(pair.second.base, pair.second.size);
    }
    g_mmaps.clear();
}

// ── Cấp phát buffer + start streaming liên tục (pre-mmap + worker thread + queue) ──
int getAllocateBuffer() {
    Stream *stream = g_stream;
    if (!stream) return -1;

    // Bước 1: Cấp phát buffer
    FrameBufferAllocator allocator(camera);
    if (allocator.allocate(stream) < 0) {
        std::cerr << "allocator.allocate() that bai\n";
        releaseCameraManager();
        return -1;
    }

    const auto &buffers = allocator.buffers(stream);
    std::cerr << "\nSo buffer duoc cap phat: " << buffers.size() << "\n";
    printPlanesInfor(buffers);

    // Bước 2: Pre-mmap tất cả buffer fd (1 lần duy nhất)
    premmap_buffers(buffers);

    // Bước 3: Tạo Request cho mỗi buffer
    std::vector<std::unique_ptr<Request>> requests;
    for (const auto &buf : buffers) {
        std::unique_ptr<Request> req(camera->createRequest());
        if (!req) {
            std::cerr << "createRequest() that bai\n";
            cleanup_mmaps();
            allocator.free(stream);
            return -1;
        }
        if (req->addBuffer(stream, buf.get()) < 0) {
            std::cerr << "addBuffer() that bai\n";
            cleanup_mmaps();
            allocator.free(stream);
            return -1;
        }
        requests.push_back(std::move(req));
    }

    // Bước 4: Tạo queue + worker thread
    std::queue<FrameJob> jobQueue;
    std::mutex qmutex;
    std::condition_variable qcv;
    std::atomic<bool> workerAlive(true);
    std::atomic<uint64_t> frameCount(0);

    // Worker: pop job → write data ra stdout (đã copy sẵn, không cần mmap)
    std::thread worker([&]() {
        while (workerAlive.load() || !jobQueue.empty()) {
            FrameJob job;
            {
                std::unique_lock<std::mutex> lk(qmutex);
                qcv.wait(lk, [&]{ return !jobQueue.empty() || !workerAlive.load(); });
                if (jobQueue.empty()) {
                    if (!workerAlive.load()) break;
                    continue;
                }
                job = std::move(jobQueue.front());
                jobQueue.pop();
            }

            if (!write_all(STDOUT_FILENO, job.data.data(), job.data.size())) {
                std::cerr << "write stdout failed, dung stream\n";
                g_running = false;
                break;
            }
        }
    });

    // Bước 5: Đăng ký handler
    CaptureHandler handler;
    handler.stream     = stream;
    handler.cam        = camera.get();
    handler.q          = &jobQueue;
    handler.qmutex     = &qmutex;
    handler.qcv        = &qcv;
    handler.frameCount = &frameCount;

    camera->requestCompleted.connect(&handler, &CaptureHandler::onRequestComplete);

    // Bước 6: Start camera và queue tất cả requests
    if (camera->start() < 0) {
        std::cerr << "camera->start() that bai\n";
        camera->requestCompleted.disconnect(&handler);
        workerAlive = false;
        qcv.notify_one();
        if (worker.joinable()) worker.join();
        cleanup_mmaps();
        allocator.free(stream);
        return -1;
    }
    camera_started = true;
    std::cerr << "Streaming started — pipe stdout to ffmpeg/ffplay. Ctrl+C to stop.\n";

    for (auto &r : requests) {
        if (camera->queueRequest(r.get()) < 0)
            std::cerr << "queueRequest() that bai\n";
    }

    // Bước 7: Chờ Ctrl+C, in FPS mỗi 3 giây
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t lastCount = 0;
    while (g_running) {
        std::this_thread::sleep_for(100ms);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
        if (elapsed >= 3000) {
            uint64_t cur = frameCount.load(std::memory_order_relaxed);
            double fps = (cur - lastCount) * 1000.0 / elapsed;
            std::cerr << "[streaming] frames=" << cur << "  fps=" << std::fixed << std::setprecision(1) << fps << "\n";
            lastCount = cur;
            lastReport = now;
        }
    }

    // ── Shutdown ──
    camera->requestCompleted.disconnect(&handler);
    camera->stop();
    camera_started = false;

    workerAlive = false;
    qcv.notify_one();
    if (worker.joinable()) worker.join();

    cleanup_mmaps();
    allocator.free(stream);

    std::cerr << "[streaming] Tong frames: " << frameCount.load() << "\n";
    return 0;
}

void printPlanesInfor(const std::vector<std::unique_ptr<FrameBuffer>> &buffers) {
    // In thông tin planes ra stderr (stdout dành cho raw data)
    for (size_t i = 0; i < buffers.size(); ++i) {
        const FrameBuffer *fb = buffers[i].get();
        const auto &planes = fb->planes();
        std::cerr << " buffer[" << i << "] planes=" << planes.size() << "\n";
        for (size_t p = 0; p < planes.size(); ++p) {
            const auto &pl = planes[p];
            std::cerr << "  plane " << p
                      << "  fd=" << pl.fd.get()
                      << "  offset=" << pl.offset
                      << "  length=" << pl.length << "\n";
        }
    }
}

// Giải phóng camera và dừng CameraManager an toàn
bool releaseCameraManager(){
        bool success = true;
        g_running = false;
        signal(SIGINT, SIG_DFL);

        if (camera) {
            if (camera_started) {
                camera->stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                camera_started = false;
            }
            g_stream = nullptr;
            camera->release();
            camera.reset();
        }

        if(cm_started) {
            // dừng CameraManager (không trả giá trị)
            cm.stop();
            cm_started = false;
        }   
        
        std::cerr << "Camera và CameraManager đã được giải phóng gọn gàng\n";
        return success;
} 

int main() {
    // Tắt buffering stdout — raw bytes phải đi thẳng ra pipe
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Đăng ký Ctrl+C
    signal(SIGINT, sigint_handler);

    // ── 1. Khởi tạo và chiếm camera ──
    if (!initCameraManager())
        return EXIT_FAILURE;

    // ── 2. Tạo và áp dụng cấu hình ──
    if (cameraConfigure() < 0) {
        releaseCameraManager();
        return EXIT_FAILURE;
    }

    // ── 3. Stream liên tục ra stdout (chờ Ctrl+C bên trong) ──
    if (getAllocateBuffer() < 0) {
        std::cerr << "streaming that bai\n";
        releaseCameraManager();
        return EXIT_FAILURE;
    }

    // ── 4. Giải phóng an toàn ──
    releaseCameraManager();
    return EXIT_SUCCESS;
} 
