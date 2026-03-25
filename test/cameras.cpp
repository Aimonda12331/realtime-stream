#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <memory>
#include <iostream>

using namespace libcamera;

//hàm cameras() giúp ta chọn camera theo id() người dùng có thể tự chọn hoặc để cho hệ thống chọn cái đầu tiên
        //Kết quả Danh sách camera tìm thấy:
                //0: /base/soc/i2c0mux/i2c@1/ov5647@36

int main() {
    //Khởi tạo CameraManager
    CameraManager cm;
    cm.start();

    //Liệt kê danh sách camera
    std::cout << "Danh sách camera tìm thấy:\n";
    int index = 0;
    for(auto const &cam : cm.cameras()) {
        std::cout << index << ": " << cam->id() << std::endl;
        index++;
    }

    if(cm.cameras().empty()) {
        std::cerr << "Không tìm thấy camera nào\n";
        return -1;
    }

    return 0;
}