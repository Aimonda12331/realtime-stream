// g++ -std=c++17 -O2 4_allocate_buffers.cpp -o 4_allocate_buffers `pkg-config --cflags --libs libcamera` -pthread
#include <iostream>
#include <iomanip>
#include <thread>
#include <memory>
#include <chrono>
#include <unistd.h>
#include <stdlib.h>

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

bool initCameraManager ();
int  cameraConfigure ();
int  pixelFormating (CameraConfiguration *config); //khai bao config trong prototype
int  getAllocateBuffer ();
void printPlanesInfor(const std::vector<std::unique_ptr<FrameBuffer>> &buffers);
bool releaseCameraManager ();

//start
bool initCameraManager () {
    cm.start();

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
    sc.size = Size(640, 480);

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

// Cấp phát buffer
int getAllocateBuffer() {
    Stream *stream = g_stream;
    {
        //Đặt allocator trong scope riêng để hủy trước khi release camera
        FrameBufferAllocator allocator(camera);

        if(allocator.allocate(stream) < 0) {
            std::cerr << "allocator.allocate() that bai\n";
            releaseCameraManager();
            return -1;
        }
        const auto &buffer = allocator.buffers(stream);
        std::cout << "\n So buffer duoc cap phat: " << buffer.size() << "\n";

        printPlanesInfor(allocator.buffers(stream));

         // free rõ ràng (optional — destructor cũng tự free)
        allocator.free(stream); // ← allocator bị hủy ở đây, trước release camera
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

        if (camera) {
            if (camera->stop() < 0) {
                std::cerr << "Warning: camera->stop() returned error\n";
                success = false;
            }

            if (camera->release() < 0) {
                std::cerr << "Warning: camera->release() returned error\n";
                success = false;
            }

            camera.reset();
        }

        // dừng CameraManager (không trả giá trị)
        cm.stop();

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

    // ── 3. Cấp phát buffer ──
    getAllocateBuffer();

     // giữ chương trình chạy cho tới Ctrl+C
    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 5. Giải phóng chương trình ──
    // giải phóng an toàn
    releaseCameraManager();
    return EXIT_SUCCESS;
} 
