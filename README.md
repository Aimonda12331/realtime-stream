# ff-libva-libcamera_0

Tệp README ngắn (Tiếng Việt) mô tả chính xác chương trình hiện tại — những gì nó làm và những gì chưa có.

Mô tả chung
------------
Đây là một ứng dụng C++ dùng `libcamera` để lấy khung hình từ camera (Raspberry Pi / V4L2-compatible) và dùng FFmpeg (libav*) để mã hóa và phát trực tiếp (stream) video qua RTSP. Ứng dụng nhẹ, hướng tới môi trường nhúng và đã được tổ chức thành các module: camera, streamer (FFmpegStreamer) và app.

Tính năng hiện có
-----------------
- Khởi tạo camera bằng `libcamera` và cấu hình độ phân giải, fps, pixel format (YUV420 hoặc NV12).
- Mã hóa video sang H.264 (sử dụng phần cứng nếu có: `h264_v4l2m2m`, hoặc phần mềm fallback) và xuất tới một RTSP server bằng `AVFormatContext`.
- Tự động chọn pixel format encoder phù hợp và thực hiện chuyển đổi màu (sws_scale) khi cần.
- Hỗ trợ override file cấu hình YAML (`config.yaml`) và override URL RTSP từ dòng lệnh.
- Xếp hàng (queue) các khung ảnh đầu vào, xử lý đa luồng để mã hóa (encoder thread) và drop frame nếu queue đầy.
- Xử lý SIGINT (Ctrl+C) để gọi `stop()`/`close()` dọn dẹp an toàn.

Những gì chương trình **không** làm (hiện tại)
------------------------------------------------
- Chưa có tích hợp âm thanh (micro INMP441 / I2S / ALSA). Audio chưa được multiplex vào RTSP stream.
- Không có giao diện web hay server RTSP nội bộ; chương trình đóng vai trò client/producer, gửi dữ liệu tới RTSP server được chỉ định qua URL.

Vị trí mã nguồn chính
----------------------
- `src/app/main.cpp` : điểm vào chương trình, đọc cấu hình, khởi tạo `CameraStreamer`.
- `src/camera/` : mã liên quan tới libcamera và lấy khung hình.
- `src/stream/streamer.cpp`, `include/streamer.h` : lớp `FFmpegStreamer` chịu trách nhiệm mã hóa và gửi RTSP.
- `include/app_config.h` : mô tả cấu trúc cấu hình YAML (AppConfig).

Phụ thuộc (cần có trên hệ để build)
-----------------------------------
- cmake, g++/clang, build-essential
- libcamera (dev headers), FFmpeg dev libs: libavformat, libavcodec, libavutil, libswscale
- (Nếu muốn thêm audio sau này) libswresample, libasound (ALSA)

Script cài đặt tự động
----------------------
Project có kèm một script cài đặt tự động `setup.sh` để cài các package và build project (hỗ trợ Raspberry Pi OS / Debian / Ubuntu). Thay vì cài tay từng package, bạn có thể chạy script này.

Chạy script (không dùng root):

```bash
chmod +x setup.sh
./setup.sh
```

Một vài lưu ý:
- Script sẽ kiểm tra môi trường và sẽ không chạy nếu bạn đang là root.
- Script sử dụng `apt` để cài gói; chỉ chạy trên hệ Debian-based.
- Script có các bước tương tác (ví dụ hỏi có muốn `full-upgrade` hay xóa thư mục `build` nếu tồn tại).
- Nếu bạn muốn chỉ cài dependencies thủ công, vẫn có thể dùng các lệnh apt trong script (mở file `setup.sh` để xem danh sách gói).

Build
-----
Từ gốc project:
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
# binary thường nằm trong build/src/app/
```

Cấu hình và cách chạy
---------------------
- File cấu hình mặc định: `config.yaml` (nằm ở thư mục project hoặc parent). Chương trình sẽ thử `config.yaml`, `../config.yaml`, `../../config.yaml` theo thứ tự.
- Ví dụ cấu hình tối thiểu (YAML):
```yaml
camera:
  width: 640
  height: 480
  fps: 30
  pixel_format: "YUV420"
  hflip: false
  vflip: false

stream:
  enabled: true
  url: "rtsp://192.168.1.100:8554/live/stream"
```

Chạy:
```bash
./app [config.yaml] [rtsp_override]
```
- Nếu `argv[1]` bắt đầu bằng `rtsp://` hoặc `rtsps://` thì nó sẽ được hiểu là URL RTSP override.
- Nếu `argv[1]` không phải URL thì coi là đường dẫn tới file cấu hình.
- `argv[2]` (nếu có) luôn ghi đè URL RTSP.

Debug / Kiểm tra nhanh
---------------------
- Kiểm tra camera libcamera: `libcamera-hello` hoặc `libcamera-vid`.
- Nếu không stream được, kiểm tra URL RTSP server, firewall, và log stderr của chương trình.

Muốn mở rộng (gợi ý)
---------------------
- Thêm audio: cần bật I2S/ASoC trên Pi, kiểm tra `arecord -l`, sau đó thêm AVStream audio và thread thu ALSA/encode AAC trong `FFmpegStreamer`.
- Thêm cấu hình audio vào `config.yaml` (device, enabled, sample_rate, bitrate) và chuyển các tham số đó vào streamer.



