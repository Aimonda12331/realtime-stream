// - Allocator phải được hủy trước khi gọi cam->release() và cm.stop().
// → Bạn đã đặt allocator trong scope riêng ({ ... }) nên khi thoát scope, destructor sẽ chạy, giải phóng buffer và fd.
// - Sau đó mới gọi:
// cam->release();
// cam.reset();       // bỏ shared_ptr
// cm.stop();
// - Như vậy camera sẽ được giải phóng hoàn toàn, không còn cảnh báo “Removing media device while still in use”.


#include <iostream>
#include <memory>
#include <libcamera/camera_manager.h>
#include <libcamera/camera.h>

using namespace libcamera;

int main() {
    CameraManager cm;
    cm.start();

    if (cm.cameras().empty()) {
        std::cerr << "Không tìm thấy camera\n";
        return -1;
    }

    std::shared_ptr<Camera> cam = cm.cameras()[0];
    cam->acquire();

    auto config = cam->generateConfiguration({ StreamRole::VideoRecording });
    if (!config) {
        std::cerr << "Không tạo được cấu hình\n";
        cam->release();
        cm.stop();
        return -1;
    }

    StreamConfiguration &cfg = config->at(0);
    std::cout << "Pixel format: " << cfg.pixelFormat.toString()
              << " size=" << cfg.size.toString() << std::endl;

    cam->release();
    cam.reset();
    cm.stop();
    return 0;
}


//Pixel format: YUV420 size=1920x1080
