═══════════════════════════════════════════════════════
  KẾ HOẠCH PHÁT TRIỂN: libcamera → libav (FFmpeg libraries) → RTSP
═══════════════════════════════════════════════════════

[✅ Test 1] Kiểm tra danh sách camera
  Mục tiêu : CameraManager thấy camera.
  Lệnh     : libcamera-hello --list-cameras
  Kết quả  : In ra số camera / tên pipeline.
  Trạng thái: DONE — "Co 1 camera kha dung"

──────────────────────────────────────────────────────

[✅ Test 2] Acquire-only
  Mục tiêu : Gọi camera->acquire() và release an toàn.
  File     : 2_camera_manager.cpp
  Kết quả  : Log libcamera + "Camera và CameraManager đã được giải phóng"
  Trạng thái: DONE

──────────────────────────────────────────────────────

[✅ Test 3] Configure & hiển thị cấu hình
  Mục tiêu : Tạo CameraConfiguration, validate(), configure(),
             in pixelFormat + kích thước.
  File     : 3_configure.cpp
  Kết quả  : "Configured: YUV420 640x480"
  Trạng thái: DONE

──────────────────────────────────────────────────────

[✅ Test 4] Allocate buffers & hiển thị plane info
  Mục tiêu : Dùng FrameBufferAllocator, in fd/offset/length.
  File     : 4_allocate_buffers.cpp
  Kết quả  : 4 buffers × 3 planes, fd/offset/length đúng
  Trạng thái: DONE

──────────────────────────────────────────────────────

[✅ Test 5] Capture 1 frame → ghi file
  Mục tiêu : createRequest, queueRequest, requestCompleted,
             mmap plane, ghi frame0.yuv.
  File     : 5_Capture_frame.cpp
  Kết quả  : frame0.yuv 460800 bytes (YUV420 640x480)
             ffplay hiển thị hình OK
  Trạng thái: DONE

──────────────────────────────────────────────────────

[🔄 Test 6] Capture liên tục → preview local
  Mục tiêu : Ghi YUV420 ra stdout liên tục (re-queue mỗi frame),
             pipe vào ffplay để xem live preview.
  File     : 5_Capture_frame.cpp (sửa CaptureHandler + re-queue)
  Lệnh     :
    ./5_capture_frame | ffplay -f rawvideo -pixel_format yuv420p -vf "hflip,vflip" -video_siz 640x480 -framerate 25 -i pipe:0
  Kết quả mong đợi: Cửa sổ ffplay hiển thị camera live
  Trạng thái: IN PROGRESS

──────────────────────────────────────────────────────

[⏳ Test 7] Encode H.264 → lưu file local (libav API)
  Mục tiêu : Thay pipeline terminal bằng thư viện libav (libavcodec/libavformat),
             encode frame YUV → H.264, lưu file `output.mp4` để kiểm tra.
  File     : modules/encoder/FFmpegEncoder.cpp (skeleton)
  Ghi chú   : Bắt đầu với `libx264` (software) để xác minh luồng,
             sau đó chuyển sang HW (h264_v4l2m2m) nếu cần.
  Kết quả mong đợi: `output.mp4` phát lại được

──────────────────────────────────────────────────────

[⏳ Test 8] Stream RTSP lên server (libav-based)
  Mục tiêu : Dùng `FFmpegEncoder` (libav) để encode và mux trực tiếp
             tới RTSP server (sử dụng libavformat/rtsp ouput hoặc pipe tới ffmpeg nếu cần).
  File     : modules/encoder/FFmpegEncoder.cpp + app integration
  Lệnh thử  : (nếu dùng terminal để test) vẫn có thể dùng: 
    ./5_capture_frame | ffmpeg ... -f rtsp "rtsps://..."
  Kết quả mong đợi: Server nhận stream, xem được bằng VLC

──────────────────────────────────────────────────────

[⏳ Test 9] Tích hợp libav vào C++ (popen -> libav API)
  Mục tiêu : Loại bỏ `popen()`/stdin piping; chương trình C++ gọi trực tiếp
             API libav để tạo `AVFormatContext` → `AVCodecContext`, gửi `AVPacket`.
  File     : modules/encoder/FFmpegEncoder.cpp (init/encode/mux API)
  Kết quả mong đợi: Ứng dụng tự encode và stream (không phụ thuộc shell ffmpeg)

──────────────────────────────────────────────────────

[⏳ Test 10] Audio + OSD hoàn chỉnh
  Mục tiêu : Thêm audio capture (ALSA/arecord) và mix audio + video trong libav,
             overlay timestamp bằng filtergraph trước khi encode.
  Flow     :
    CameraCapture (thread) → frame queue (YUV) → FFmpegEncoder (video)
    AudioCapture  (thread) → audio queue (PCM) → FFmpegEncoder (audio)
    FFmpegEncoder → RTSP server (muxed H264 + AAC)
  Kết quả mong đợi: Stream đầy đủ video + audio lên server

──────────────────────────────────────────────────────

[⏳ Test 11] Viết lại theo hướng đối tượng (OOP) & tích hợp
  Mục tiêu : Refactor toàn bộ thành các lớp rõ ràng:
    - `CameraCapture` : quản lý libcamera, allocate buffers, enqueue requests, đưa frames vào queue
    - `FFmpegEncoder`  : khởi tạo libav, nhận AVFrame từ converter, encode và mux
    - `RTSPStreamer`   : (nếu tách) quản lý kết nối/URL, retry
    - `AudioCapture`   : (nếu cần) đọc mic, convert PCM → AVFrame
    - `AppController`  : đọc cấu hình (.ini), khởi tạo các module, quản lý thread pool
  Đầu ra   : Mã nguồn có thể build bằng CMake, module tách rời, dễ test.

──────────────────────────────────────────────────────

[🧾 Hành động tiếp theo tôi sẽ làm nếu bạn đồng ý]
  1) Tạo skeleton `modules/encoder/FFmpegEncoder.{cpp,h}` với API init/encode/stop.
  2) Thêm CMakeLists nhỏ cho module và kiểm tra link `-lavformat -lavcodec -lavutil -lswscale`.
  3) Viết ví dụ tích hợp với `5_capture_frame` để encode lưu `output.mp4`.

═══════════════════════════════════════════════════════