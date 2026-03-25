// g++ -std=c++17 -O2 5_capture_frame.cpp -o 5_capture_frame `pkg-config --cflags --libs libcamera` -pthread
#include <iostream>
#include <iomanip>
#include <thread>
#include <memory>
#include <chrono>
#include <unistd.h>
#include <stdlib.h>
#include <atomic>
#include <map>

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
    std::cout << "Co " << cm.cameras().size() << " camera kha dung \n" << std::endl;

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

    // In pixel format và kích thước đã áp dụng
    StreamConfiguration &applied = config->at(0);
    std::cout << "Configured: " << applied.pixelFormat.toString()
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
    std::cout << "[pixelFormating] Format mặc định: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";

    sc.pixelFormat = formats::YUV420;
    sc.size = Size(1920, 1080);

    CameraConfiguration::Status st = config->validate();
    
    if(st == CameraConfiguration::Invalid) { //Check YUV ko dc check NV12
        std::cerr << "Cau hinh khong hop le voi YUV420, thu NV12\n";
        sc.pixelFormat = formats::NV12;
        st = config->validate();
    } else if( st == CameraConfiguration::Adjusted) {
        std::cout << "config duoc dieu chinh\n";
        return -1;
    }

    if (st == CameraConfiguration::Adjusted) {
        std::cout << "[pixelFormating] Config được điều chỉnh → "
                  << sc.pixelFormat.toString()
                  << " " << sc.size.width << "x" << sc.size.height << "\n";
    }

    // Log format cuối cùng được áp dụng
    std::cout << "[pixelFormating] Format cuối: "
              << sc.pixelFormat.toString()
              << " " << sc.size.width << "x" << sc.size.height << "\n";
    return 0;
}

// Handler được đăng ký với sự kiện requestCompleted của camera.
// Lưu ý: handler này phải sống đủ lâu cho tới khi bạn gọi disconnect().
struct CaptureHandler {
    Stream *stream = nullptr;   // Stream mà handler quan tâm
    bool    gotFrame = false;   // Cờ đã ghi frame thành công (tránh xử lý lại)

    // Hàm callback được gọi khi một Request hoàn thành
    void onRequestComplete(Request *request) {
        if (gotFrame) return;   //nếu đã có frame thì bỏ qua

        // Lấy map các buffer theo Stream* từ Request
        const auto &bufferMap = request->buffers();
        auto it = bufferMap.find(stream);
        if (it == bufferMap.end()) return;  // Request không chứa stream này
        FrameBuffer *fb = it->second;       // Lấy FrameBuffer tương ứng

        const auto &planes = fb->planes();
        if (planes.empty()) return;     // Không có plane → nothing to do

        // Tính kích thước lớn nhất cần mmap để bao phủ tất cả plane
        // (offset + length) cho mỗi plane; lấy max để mmap 1 lần
        size_t totalSize = 0;
        for (const auto &pl : planes)
            totalSize = std::max(totalSize, (size_t)(pl.offset + pl.length));

        // Lấy fd của plane 0 và mmap vùng nhớ (LƯU Ý: nếu mỗi plane có fd khác nhau,
        // bạn phải mmap từng plane bằng fd và offset tương ứng — không dùng fd[0] chung)
        int fd = planes[0].fd.get();
        void *base = mmap(nullptr, totalSize, PROT_READ, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) { std::perror("mmap"); return; }

        // Ghi từng plane vào file đầu ra (interleaving/format tùy pixel format)
        FILE *out = fopen("frame0.yuv", "wb");
        if (out) {
            for (const auto &pl : planes)
                // Viết dữ liệu plane: base + offset, độ dài = pl.length
                fwrite(static_cast<uint8_t*>(base) + pl.offset, 1, pl.length, out);
            fclose(out);
            std::cout << "Da ghi frame0.yuv thanh cong\n";
        } else {
            std::perror("fopen frame0.yuv");
        }

        //Giải phóng vũng mmap đã cấp
        munmap(base, totalSize);
        //Đánh dấu đã lấy frame để ngăn xử lý tiếp
        gotFrame = true;
    }
};

// Cấp phát buffer
int getAllocateBuffer() {
    Stream *stream = g_stream;
    if(!stream) return -1;

        // ── Bước 1: Cấp phát buffer (giữ allocator sống tới khi stop) ──
        FrameBufferAllocator allocator(camera);
        if(allocator.allocate(stream) < 0) {
            std::cerr << "allocator.allocate() that bai\n";
            releaseCameraManager();
            return -1;
        }

        const auto &buffer = allocator.buffers(stream);
        std::cout << "\n So buffer duoc cap phat: " << buffer.size() << "\n";

        printPlanesInfor(allocator.buffers(stream));

        // ── Bước 2: Tạo Request cho mỗi buffer ──
        std::vector<std::unique_ptr<Request>> requests;
        for(const auto &buf : buffer) {    
            std::unique_ptr<Request> req(camera->createRequest());
            if(!req) {
                std::cerr << "createRequest() that bai\n";
                allocator.free(stream);
                return -1;
            }

            if(req->addBuffer(stream, buf.get()) < 0) {
                std::cerr << "addBuffer() that bai\n";
                allocator.free(stream);
                return -1;
            }
            requests.push_back(std::move(req));
        }

    // ── Bước 3: Đăng ký callback ──
    CaptureHandler handler;
    handler.stream = stream;
    camera->requestCompleted.connect(&handler,
                                     &CaptureHandler::onRequestComplete);
                                     
    // ── Bước 4: Start camera và queue requests ──
    if(camera->start() < 0) {
        std::cerr << "camera->start() that bai";
        camera->requestCompleted.disconnect(&handler);
        return -1;
    }
    camera_started = true; // đánh dấu
    
    for(auto &r : requests) {
        if(camera->queueRequest(r.get()) < 0) 
            std::cerr << "queueRequest() that bai\n";
        } 
       
    for (int i = 0; i < 100 && !handler.gotFrame; ++i) 
        std::this_thread::sleep_for(50ms);

        //disconnect() trước stop() sau (không làm ngược lại)
        camera->requestCompleted.disconnect(&handler);
        camera->stop();
        camera_started = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // chờ callback đang chạy

        allocator.free(stream);

    if (!handler.gotFrame) {
        std::cerr << "Timeout: khong nhan duoc frame nao\n";
        return -1;
    }
    return 0;
}

void printPlanesInfor(const std::vector<std::unique_ptr<FrameBuffer>> &buffers) {
    //In thông tin planes ──
        for (size_t i = 0; i < buffers.size(); ++i) {
            const FrameBuffer *fb = buffers[i].get();
            const auto &planes = fb->planes();
            std::cout << " buffer[" << i << "] planes=" << planes.size() << "\n";
            for (size_t p = 0; p < planes.size(); ++p) {
                const auto &pl = planes[p];
                std::cout << "  plane " << p
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
        
        std::cout << "Camera và CameraManager đã được giải phóng gọn gàng\n";
        return success;
} 

int main() {
  // đăng ký Ctrl+C trước
    signal(SIGINT, sigint_handler);

    // ── 1. khởi tạo và chiếm camera một lần
    if (!initCameraManager())
        return EXIT_FAILURE;

   // Thêm dòng này để configure và in formatPixel
   // ── 2. Tạo và áp dụng cấu hình ──
    if (cameraConfigure() < 0) {
        releaseCameraManager();
        return EXIT_FAILURE;
    }

    // ── 3. Capture 1 frame và ghi ra frame0.yuv ──
    if (getAllocateBuffer() < 0) {
        std::cerr << "captureOneFrame() that bai\n";
        releaseCameraManager();
        return EXIT_FAILURE;
    }

     // giữ chương trình chạy cho tới Ctrl+C
    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 5. Giải phóng chương trình ──
    // giải phóng an toàn
    releaseCameraManager();
    return EXIT_SUCCESS;
} 
