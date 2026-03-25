#include "camera.h"

#include <cstdlib>
#include <cstdio>
#include <signal.h>

#include <libcamera/formats.h>

//Con trỏ toàn cục để signal hendler gọi stop()
static CameraStreamer *g_streamer = nullptr;

static void sigint_handler(int) {
    if (g_streamer) g_streamer->stop();
}

int main() {
    //Raw bytes stdout - không buffer
    setvbuf(stdout, nullptr, _IONBF, 0);

    CameraStreamer streamer;
    g_streamer = &streamer;
    signal(SIGINT, sigint_handler);

    //1. Khởi tạo 
    if (!streamer.init())
        return EXIT_FAILURE;

    //2. Cấu hhinhf (YUV420 640x480, fallback N12 bên trong)
    if (streamer.configure(libcamera::formats::YUV420,
                           libcamera::Size(640, 480)) < 0) {
        streamer.release();
        return EXIT_FAILURE;
    }

    // Bắt đầu streaming (chặn tới khi stop() gọi)
    streamer.startStreaming();

    //4. Giải phóng
    streamer.release();
    g_streamer = nullptr;
    signal(SIGINT, SIG_DFL);

    return EXIT_SUCCESS;
}
