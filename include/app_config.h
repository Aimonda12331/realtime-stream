#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>

struct CameraConfig {
    int width = 640;
    int height = 480;
    std::string pixel_format = "YUV420"; // YUV420 | NV12
    int fps = 25;
    bool hflip = false;
    bool vflip = false;
};

struct StreamConfig {
    bool enabled = true;
    std::string url;
};

struct AppConfig {
    CameraConfig camera;
    StreamConfig stream;
};

// return true nếu load thành công; false nếu fail (giữ default)
bool loadAppConfig(const std::string &path, AppConfig &cfg, std::string &err);

#endif // APP_CONFIG_H
