//g++ -std=c++17 -O2 3_buffer_allocate.cpp -o 3_buffer_allocate `pkg-config --cflags --libs libcamera` -pthread
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
#include <libcamera/formats.h>

using namespace libcamera;
using namespace std::chrono_literals;

static CameraManager cm;   //cm là biến đại diện cho đối tượng CameraManager
static std::shared_ptr<Camera> camera; //Khai báo biến toàn cục để sau này callback có thể truy cập

//Biến toàn cục để dùng capture bằng Ctr-C
static volatile bool g_running = true;
static void sigint_handler(int) { g_running = false; }

bool initCameraManager ();
int cameraConfigure ();
int pixelFormating (CameraConfiguration *config); //khai bao config trong prototype
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

    // khởi tạo và chiếm camera một lần
    if (!initCameraManager())
        return EXIT_FAILURE;

   // Thêm dòng này để configure và in format
    if (cameraConfigure() < 0) {
        releaseCameraManager();
        return EXIT_FAILURE;
    }

    // giữ chương trình chạy cho tới Ctrl+C
    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // giải phóng an toàn
    releaseCameraManager();
    return EXIT_SUCCESS;
} 

