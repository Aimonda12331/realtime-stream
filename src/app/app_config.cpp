#include <app_config.h>
#include <yaml-cpp/yaml.h>

static bool isValidPixelFormat(const std::string &s) {
    return s == "YUV420" || s == "NV12";
}

bool loadAppConfig(const std::string &path, AppConfig &cfg, std::string &err) {
    try {
        YAML::Node root = YAML::LoadFile(path);

        if (root["camera"]) {
            YAML::Node cam = root["camera"];
            if (cam["width"])  cfg.camera.width = cam["width"].as<int>();
            if (cam["height"]) cfg.camera.height = cam["height"].as<int>();
            if (cam["hflip"])  cfg.camera.hflip = cam["hflip"].as<bool>();
            if (cam["vflip"])  cfg.camera.vflip = cam["vflip"].as<bool>();
            if (cam["pixel_format"]) cfg.camera.pixel_format = cam["pixel_format"].as<std::string>();
            if (cam["fps"])    cfg.camera.fps = cam["fps"].as<int>();
        }

        if (root["stream"]) {
            YAML::Node st = root["stream"];
            if (st["enabled"]) cfg.stream.enabled = st["enabled"].as<bool>();
            if (st["url"]) cfg.stream.url = st["url"].as<std::string>();
        }

        if (cfg.camera.width <= 0 || cfg.camera.height <= 0) {
            err = "camera.width/camera.height phai > 0";
            return false;
        }
        if (cfg.camera.fps <= 0) {
            err = "camera.fps phai > 0";
            return false;
        }
        if (!isValidPixelFormat(cfg.camera.pixel_format)) {
            err = "camera.pixel_format chi ho tro YUV420 hoac NV12";
            return false;
        }

        return true;
    } catch (const std::exception &ex) {
        err = ex.what();
        return false;
    }
}