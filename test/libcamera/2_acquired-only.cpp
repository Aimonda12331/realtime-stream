//g++ -std=c++17 -O2 2_camera_manager.cpp -o 2_camera_manager `pkg-config --cflags --libs libcamera` -pthread
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

//start
bool initCameraManager () {
    cm.start();

    if(cm.cameras().empty()) {
        std::cerr << "Không tìm thấy camera" << std::endl;
        cm.stop();
        return false; //thất bại
    }
    std::cout << "Co " << cm.cameras().size() << " camera kha dung" << std::endl;

    //Chọn camera đầu tiên
    camera = cm.cameras()[0];
    if (!camera) {
        std::cerr << "Khong the chiem doat camera\n";
        cm.stop();
        return false;
    }

    if (camera->acquire() < 0) {
        std::cerr << "Khong the chiem duoc camera\n" ;
        camera.reset();
        cm.stop();
        return false;
    }

    return true;
}


// Giải phóng camera và dừng CameraManager an toàn
void releaseCameraManager()
{
    if (camera) {
        // Stop camera if it was started (safe to call even if not started)
        camera->stop();

        // Release ownership in the driver
        if (camera->release() < 0)
            std::cerr << "Warning: camera->release() returned error\n";

        // Drop our shared_ptr reference
        camera.reset();
    }

    // Stop camera manager
    cm.stop();
    std::cout << "Camera và CameraManager đã được giải phóng\n";
}


int main() {
  // đăng ký Ctrl+C trước
    signal(SIGINT, sigint_handler);

    // khởi tạo và chiếm camera một lần
    if (!initCameraManager())
        return EXIT_FAILURE;

    // giữ chương trình chạy cho tới Ctrl+C
    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // giải phóng an toàn
    releaseCameraManager();
    return EXIT_SUCCESS;
} 

