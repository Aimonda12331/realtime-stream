#include "camera.h"
#include "streamer.h"
#include "app_config.h"

#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <iostream>
#include <string>
#include <filesystem>

#include <libcamera/formats.h>

using camera_streamer = CameraStreamer;
//Con trỏ toàn cục để signal hendler gọi stop()
static camera_streamer *g_streamer = nullptr;

static void sigint_handler(int) {
    if (g_streamer) g_streamer->stop(); 
}

static bool startsWith(const std::string &s, const std::string &prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool fileExists(const std::string &p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

int main(int argc, char **argv) {
    //Raw bytes stdout - không buffer
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Mac dinh: doc config.yaml nhaaa
    std::string config_path = "config.yaml";
    std::string rtsp_override;

    // Tuong thich kieu cu:
    // - Neu argv[1] la rtsp:// hoac rtsps:// -> coi la override RTSP
    // - Nguoc lai argv[1] la duong dan YAML
    if (argc > 1) {
        std::string a1 = argv[1];
        if (startsWith(a1, "rtsp://") || startsWith(a1, "rtsps://")) {
            rtsp_override = a1;
        } else {
            config_path = a1;
        }
    }
    if (argc > 2) {
        rtsp_override = argv[2];
    }

    AppConfig cfg;
    std::string err;
    std::string loaded_config_path = config_path;
    if (!loadAppConfig(loaded_config_path, cfg, err)) {
        // Common case: binary launched from build/src while config.yaml is in project root.
        const std::string try1 = "../" + config_path;
        const std::string try2 = "../../" + config_path;
        if (fileExists(try1) && loadAppConfig(try1, cfg, err)) {
            loaded_config_path = try1;
        } else if (fileExists(try2) && loadAppConfig(try2, cfg, err)) {
            loaded_config_path = try2;
        } else {
            std::cerr << "[config] Load YAML that bai (" << config_path << "): " << err
                      << ". Dung default values.\n";
        }
    }
    if (fileExists(loaded_config_path)) {
        std::cerr << "[config] Loaded: " << loaded_config_path << "\n";
    }

    if (!rtsp_override.empty()) {
        cfg.stream.url = rtsp_override;
        cfg.stream.enabled = true;
    }

    camera_streamer streamer;
    g_streamer = &streamer;
    signal(SIGINT, sigint_handler);

    //1. Khởi tạo 
    if (!streamer.init()) return EXIT_FAILURE;

    //2. Cấu hhinhf (YUV420 640x480, fallback N12 bên trong)
    libcamera::PixelFormat fmt = libcamera::formats::YUV420;
    if (cfg.camera.pixel_format == "NV12") {
        fmt = libcamera::formats::NV12;
    }

    streamer.setFlip(cfg.camera.hflip, cfg.camera.vflip);

    std::cerr << "[config] camera: " << cfg.camera.width << "x" << cfg.camera.height
              << " fmt=" << cfg.camera.pixel_format
              << " fps=" << cfg.camera.fps
              << " hflip=" << cfg.camera.hflip << " vflip=" << cfg.camera.vflip << "\n";
    std::cerr << "[config] stream: enabled=" << cfg.stream.enabled
              << " url=" << cfg.stream.url << "\n";

    streamer.setTargetFps(cfg.camera.fps);  // khóa Gắn FPS trước configure
    if (streamer.configure(fmt, libcamera::Size(cfg.camera.width, cfg.camera.height)) < 0) {
        streamer.release();
        return EXIT_FAILURE;
    }

    if (cfg.stream.enabled && !cfg.stream.url.empty()) {
        streamer.setRtsp(cfg.stream.url, cfg.camera.fps);
    }

    streamer.startStreaming();

    //4. Giải phóng
    streamer.release();
    g_streamer = nullptr;
    std::signal(SIGINT, SIG_DFL);
    return EXIT_SUCCESS;
}
