# realtime-stream

Checklist — README này bao gồm:
- Tổng quan dự án và kiến trúc
- Hướng dẫn build và chạy (các lệnh copy được)
- Cấu hình (`config.yaml`) và ví dụ
- Chức năng hiện tại và các sửa lỗi gần đây cho realtime
- Các bước tinh chỉnh thực tế để cải thiện realtime và giảm CPU
- Khắc phục sự cố và cách đọc log

## Tổng quan dự án

`realtime-stream` thu nhận khung hình thô từ camera Raspberry Pi bằng libcamera, tùy chọn mã hóa bằng FFmpeg (H.264) và phát RTSP stream. Code tập trung vào độ trễ thấp bằng cách dùng dmabuf pre-mapped, hàng đợi nhỏ, và encoder FFmpeg được tinh chỉnh cho low latency.

Các file nguồn chính:
- `src/app/main.cpp` — điểm vào chương trình, load `config.yaml`, khởi động `CameraStreamer`.
- `include/camera.h`, `src/camera/camera.cpp` — capture libcamera, pre-mmap, xử lý request và job queue.
- `include/streamer.h`, `src/stream/streamer.cpp` — wrapper FFmpeg (`FFmpegStreamer`) để encode và xuất RTSP.
- `src/app/app_config.cpp` — parse YAML config.
- `config.yaml` — ví dụ config mặc định.

## Chức năng hiện tại
- Phát hiện và lấy camera libcamera đầu tiên.
- Tạo cấu hình `VideoRecording` và áp dụng pixel format + size (fallback sang NV12 nếu cần).
- Pre-allocate và pre-mmap dmabuf để tránh overhead mmap mỗi frame.
- Sao chép plane frame vào pool buffer CPU tái sử dụng để tránh cấp phát liên tục.
- Chạy worker thread đẩy frame vào `FFmpegStreamer` khi RTSP bật, hoặc ghi raw ra stdout nếu không.
- `FFmpegStreamer` encode frame trong thread riêng và publish RTSP qua libav*.

### Các sửa lỗi quan trọng gần đây
- Giảm kích thước queue nội bộ (camera job queue và encoder queue = 2) để hạn chế buffering và giảm latency.
- Sửa PTS: frame dùng wall-clock PTS và đảm bảo tăng đơn điệu để tránh lỗi muxer.
- Set `AVFMT_FLAG_FLUSH_PACKETS` và `pb->direct = 1` để giảm buffering nội bộ FFmpeg.
- Dùng `av_interleaved_write_frame` để flush ngay.
- Ưu tiên encoder phần cứng Pi `h264_v4l2m2m`, fallback sang phần mềm nếu không có.
- Tái sử dụng buffer pool để tránh churn heap.
- Giảm GOP xuống ~0.5s và tắt B-frame để giảm biến thiên latency.

## Build

Yêu cầu:
- Toolchain C++17, CMake, libcamera dev, thư viện FFmpeg (libavcodec, libavformat, libswscale, libavutil), yaml-cpp.

Build điển hình:
```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

Binary sau build: `build/bin/bodycam`.

## Run

Chạy với `config.yaml` mặc định:
```bash
./build/bin/bodycam config.yaml
```

Override RTSP URL:
```bash
./build/bin/bodycam rtsps://your-server/stream
```

Nếu `stream.enabled` = false, chương trình ghi raw ra stdout. Ví dụ xem bằng ffplay:
```bash
./build/bin/bodycam > /tmp/out.yuv &
ffplay -f rawvideo -pixel_format yuv420p -video_size 640x480 -framerate 15 /tmp/out.yuv
```

Xem RTSP output:
```bash
ffplay -rtsp_transport tcp -i "rtsps://server/stream?secretKey=..."
```

## Cấu hình (`config.yaml`)

Ví dụ:
```yaml
camera:
  width: 640
  height: 480
  pixel_format: YUV420
  fps: 15
  hflip: true
  vflip: true

stream:
  enabled: true
  url: "rtsps://vcloud.vcv.vn:20972/livestream/UBTL.24D2AC5_TEST1?secretKey=abcd1234"
```

Ý nghĩa:
- `camera.width/height` — độ phân giải capture.
- `camera.pixel_format` — định dạng pixel (YUV420 hoặc NV12).
- `camera.fps` — FPS cảm biến.
- `hflip/vflip` — lật hình.
- `stream.enabled/url` — thiết lập RTSP.

## Cách cải thiện realtime và giảm CPU

1. Ưu tiên encoder phần cứng (`h264_v4l2m2m`).
2. Nếu dùng phần mềm:
    - Giảm bitrate (ví dụ 800k).
    - Dùng preset `ultrafast`.
    - Giảm độ phân giải hoặc fps.
3. Tránh chuyển đổi màu: chọn pixel format phù hợp.
4. Giữ queue nhỏ (1–2).
5. Tinh chỉnh OS:
    - Set CPU governor = performance.
    - Chạy với realtime scheduling (`chrt`).
6. Giảm logging.
7. Zero-copy nâng cao: dùng dmabuf trực tiếp cho encoder.

## Khắc phục sự cố

- `enc_fps << capture_fps` — encoder không theo kịp.
- `non monotonically increasing dts` — lỗi PTS.
- `Frame size too small` — mismatch format/size.
- Vấn đề RTSP với `rtsps://` — nên dùng TCP.

## Log cần chú ý
- `[config]` — cấu hình camera/stream.
- `[FPS]` — FPS cảm biến.
- `[Streaming]` — capture vs encode vs send.
- `[RTSP] Encoder selected:` — encoder phần cứng hay phần mềm.
