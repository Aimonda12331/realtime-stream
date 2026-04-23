Pipeline chạy thử:
<rpicam-vid --nopreview -t 0 --codec h264 --inline --framerate 25 --bitrate 6000000 --width 720 --height 640 --flush -o - | ffmpeg -fflags +genpts -fflags nobuffer -flags low_delay -f h264 -i - -vf "drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:text='%{pts\\:hms}':x=10:y=10:fontsize=24:fontcolor=white:box=1:boxcolor=0x00000080"   -pix_fmt yuv420p -c:v h264_v4l2m2m -b:v 2000k   -rtsp_transport udp -muxdelay 0.1 -f rtsp "rtsps://vcloud.vcv.vn:20972/livestream/UBTL.24D2AC5_TEST1?secretKey=abcd1234">

libcamera nằm ở <ls /usr/include/libcamera/libcamera/libcamera.h>

| ffplay -f rawvideo -pixel_format yuv420p -vf "hflip,vflip" -video_siz 640x480 -framerate 25 -i pipe:0

Thư viện cài dặt
    + libcamera-dev

<ini giống yaml>
git clone https://github.com/benhoyt/inih.git
    ini.h
    ini.c

<Cây thư mục>

ff-libva/
├── CMakeLists.txt          # Root CMakeLists.txt
├── README.md               # Docs
├── thread.md               # Docs
├── include/                # Headers
│   ├── audio/
│   │   └── audio.h
│   ├── camera/
│   │   └── libcamera.h
│   └── config/             # Empty
├── src/                    # Main executable
│   ├── CMakeLists.txt
│   └── main.cpp
├── modules/                # Libraries
│   ├── CMakeLists.txt
│   ├── audio/
│   │   ├── CMakeLists.txt
│   │   └── audio.cpp
│   ├── camera/
│   │   ├── CMakeLists.txt
│   │   └── libcamera.cpp
│   └── config/
│       └── CMakeLists.txt
├── test/                   # Test files
└── third-party/            # Third-party libs

Ví dụ flow hoàn chỉnh: Camera thread capture → FFmpeg thread encode → RTSP thread stream → Audio thread mix. Đợi xong để thiết kế thread pool đúng cách.
 
Pixel format: YUV420 size=1920x1080




bodycam sẽ lưu trước 5 phut ke tu khi bam nut



Kế hoạch ngắn:
1. Tổng quan nhanh: mục đích chương trình và môi trường chạy.
2. Những thư viện/API và tính năng chính đang được dùng.
3. Luồng xử lý (step-by-step) trong các hàm chính (`init`, `configure`, `startStreaming`, `onRequestComplete`, ...).
4. Cách chạy/pipe đầu ra sang ffmpeg/ffplay + ví dụ lệnh.
5. Vấn đề cần lưu ý và gợi ý sửa/ổn định.

1) Mục đích & môi trường chạy
- File: `src/camera.cpp` — lớp `CameraStreamer`.
- Mục đích: bật camera (libcamera), cấu hình luồng video, cấp buffer (dmabuf), mmap trước các fd, sao chép dữ liệu frame vào hàng đợi, rồi ghi dữ liệu frame ra STDOUT để pipe cho ffmpeg/ffplay. Dùng cho Linux (Raspberry Pi etc.). Không chạy trực tiếp trên Windows vì dùng POSIX API và `libcamera`.

2) Thư viện / API / headers đang dùng
- libcamera C++ API (namespace `libcamera`): các lớp như `CameraManager`, `Camera`, `CameraConfiguration`, `StreamConfiguration`, `FrameBufferAllocator`, `Request`, `FrameBuffer`, `PixelFormat`, `formats::NV12`, `StreamRole::VideoRecording`, ...
- C++ Standard Library: `std::thread`, `std::mutex`, `std::condition_variable`, `std::unique_ptr`, `std::atomic`, `std::vector`, `std::queue`, `std::chrono`, `std::cerr`...
- POSIX system calls / headers:
    - `<unistd.h>`: `write`, `STDOUT_FILENO`
    - `<sys/mman.h>`: `mmap`, `munmap`, `MAP_SHARED`, `PROT_READ`
    - `<signal.h>` (bao gồm but not used heavily trong file)
    - `errno`, `strerror` (đọc lỗi)
- Kiến trúc tổng thể: producer (callback `onRequestComplete`) tạo `FrameJob`, consumer (worker thread) ghi frame ra STDOUT. Camera buffer được tái sử dụng bằng `request->reuse(Request::ReuseBuffers)`.

3) Luồng xử lý — các hàm chính (mô tả bước‑bước)
- `CameraStreamer::init()`
    - gọi `cm_.start()` để chạy `CameraManager`.
    - lấy danh sách camera (`cm_.cameras()[0]`) và `acquire()` camera đầu tiên.
    - Trả về true nếu thành công.

- `CameraStreamer::configure(PixelFormat fmt, Size size)`
    - tạo `CameraConfiguration` bằng `generateConfiguration({ StreamRole::VideoRecording })`.
    - gọi `applyPixelFormat(config.get(), fmt, size)` để set định dạng và size, fallback NV12 nếu cần.
    - gọi `camera_->configure(config.get())`.
    - lưu `stream_ = applied.stream()`.

- `CameraStreamer::applyPixelFormat(...)`
    - cố gắng đặt `sc.pixelFormat = fmt` và `sc.size = size`.
    - gọi `config->validate()`; nếu Invalid thì thử thay bằng `formats::NV12`.
    - ghi log format cuối cùng.

- `CameraStreamer::startStreaming()`
    - Bước 1: dùng `FrameBufferAllocator` để `allocate(stream_)`. Lấy `buffers = allocator.buffers(stream_)`.
    - Bước 2: `premapBuffers(buffers)` — duyệt từng plane của mỗi buffer, lấy `pl.fd` (dmabuf fd) và mmap các fd đó (lưu vào `mmaps_`).
    - Bước 3: Tạo `Request` cho mỗi `FrameBuffer` và `addBuffer(stream_, buf.get())`.
    - Bước 4: Tạo worker thread — thread này chờ `jobQueue_` (condition_variable) và ghi toàn bộ bytes của `FrameJob` ra `STDOUT_FILENO` bằng `writeAll()`.
    - Bước 5: kết nối callback `camera_->requestCompleted.connect(this, &CameraStreamer::onRequestComplete)`.
    - Bước 6: `camera_->start()`, mark `cameraStarted_ = true`.
    - Queue mọi request (`camera_->queueRequest(r.get())`).
    - Bước 7: Vòng lặp chính monitoring FPS (in ra mỗi ~3s) và chờ `running_` chuyển false (khi `stop()` được gọi).
    - Shutdown: disconnect callback, `camera_->stop()`, dừng worker, `cleanupMmaps()`, `allocator.free(stream_)`.

- `CameraStreamer::onRequestComplete(Request *request)`
    - kiểm tra `running_`.
    - lấy buffer tương ứng với `stream_` từ `request->buffers()`.
    - duyệt các `planes` của `FrameBuffer`.
    - tính tổng `totalLen` = sum(pl.length).
    - tạo `FrameJob job` và `memcpy` từ vùng đã mmap (trong `mmaps_`) vào `job.data`. Mỗi plane copy nối tiếp nhau.
    - `request->reuse(Request::ReuseBuffers)` và `queueRequest(request)` để requeue buffer.
    - đẩy `job` vào `jobQueue_` và notify worker; tăng `frameCount_`.

- `CameraStreamer::premapBuffers(...)`
    - duyệt mọi plane, dùng `pl.fd.get()` làm key trong `mmaps_`, cập nhật `entry.size` = max(end).
    - gọi `mmap(nullptr, entry.size, PROT_READ, MAP_SHARED, fd, 0)` cho mỗi fd; lưu `entry.base` = pointer; log thành công/không thành công.

- `CameraStreamer::cleanupMmaps()` — `munmap` mọi `entry.base` rồi clear `mmaps_`.

- `CameraStreamer::writeAll(int fd, const void *buf, size_t size)` — loop write tới khi hết bytes hoặc lỗi.

4) Cách chạy và pipe sang ffplay/ffmpeg
- Ứng dụng in raw frame bytes vào STDOUT. Bạn nên pipe stdout sang ffplay/ffmpeg và cho biết định dạng & kích thước chính xác (program in ra khi configure thành công, ví dụ "Configured: NV12 640x480").
- Ví dụ (giả sử file thực thi của bạn là `bodycam` và định dạng cuối cùng là NV12, size 640x480, framerate 30):

    - Dùng ffplay:
      ./bodycam | ffplay -f rawvideo -pixel_format nv12 -video_size 640x480 -framerate 30 -i -

    - Dùng ffmpeg để encode lưu file:
      ./bodycam | ffmpeg -f rawvideo -pixel_format nv12 -video_size 640x480 -framerate 30 -i - -c:v libx264 output.mp4

- Lưu ý: Nếu format là YUV420 (I420 / yuv420p), đổi `-pixel_format yuv420p`.

5) Những điểm cần lưu ý / vấn đề tiềm ẩn và gợi ý cải tiến
- Môi trường: mã dùng libcamera + POSIX (`mmap`, `write`), chạy trên Linux (ví dụ Raspberry Pi). Không chạy trên Windows.
- mmap & offset:
    - Hiện tại `premapBuffers` mmap(fd, entry.size, ..., offset=0) và khi copy dùng `mit->second.base + pl.offset`. Về lý thuyết mmap offset phải là bội của page size. Mã đang map từ offset 0 — điều này thường ổn nếu fd là một buffer có thể map toàn bộ. Tuy nhiên nếu cần map phần nhỏ bắt đầu tại `pl.offset`, cách an toàn hơn là:
        - map từ `map_offset = pl.offset & ~(pagesize-1)` với length = (pl.offset - map_offset) + pl.length
        - sau đó copy từ base + (pl.offset - map_offset)
    - Nếu kernel/driver kỳ quặc, mapping toàn bộ fd có thể fail; tốt nhất nên xử lý lỗi mapping rõ ràng (hiện có log) và tránh memcpy từ nullptr.
- Kiểm tra `mmaps_[fd].base != nullptr` trước khi memcpy — hiện code bỏ qua plane nếu fd không có trong mmaps_, nhưng nếu mmaps_[fd] tồn tại nhưng base == nullptr thì vẫn memcpy? Code hiện kiểm tra `if (mit == mmaps_.end()) continue;` nhưng không kiểm tra `mit->second.base` trước memcpy. Nên kiểm tra và skip nếu base == nullptr.
- Thread lifetime & ownership:
    - Callback `camera_->requestCompleted.connect(this, ...)` giữ con trỏ `this`. Đảm bảo đối tượng `CameraStreamer` tồn tại đến khi disconnect; hiện `release()` gọi `camera_->requestCompleted.disconnect(this)` ở những chỗ thích hợp nhưng cần cẩn thận nếu object bị hủy trong khi callback đang chạy.
- STDOUT binary:
    - Ghi thẳng ra STDOUT có thể gây rối nếu bạn chạy trong terminal. Luôn pipe vào chương trình nhận (ffplay/ffmpeg). Có thể thay STDOUT bằng FIFO hoặc socket nếu thích.
- Kiểm tra lỗi return value: nhiều chỗ log nhưng vẫn tiếp tục; có thể muốn dọn dẹp kỹ hơn khi lỗi xảy ra (ex: nếu một mmap thất bại, có thể dừng streaming).
- Đồng bộ queue shutdown:
    - Worker thread loop chờ `workerAlive_ || !jobQueue_.empty()`. Khi dừng, `workerAlive_` được false và `queueCv_.notify_one()` được gọi. Hiện logic có vẻ hợp lý, nhưng cần xác nhận `queueCv_.wait` điều kiện và notify đủ để wake worker khi shutting down.
- Tối ưu: tránh sao chép (memcpy) nếu có thể — dùng dmabuf zero-copy tới client nếu client hỗ trợ (khó hơn). Hiện cách sao chép an toàn và đơn giản.

6) Cách build (gợi ý)
- Dự án dùng CMake (có `CMakeLists.txt`). Ví dụ build trên Linux:
  mkdir -p build
  cd build
  cmake ..
  make -j
- Cần cài `libcamera` dev headers/libraries và link theo CMakeLists. Cần pthreads (std::thread).

7) Debugging tips
- Nếu chương trình không tìm camera: kiểm tra `libcamera-hello` có hoạt động không trên máy.
- Nếu `allocator.allocate()` thất bại: kiểm tra quyền truy cập / cấu hình kernel driver.
- Kiểm tra log "Configured: ..." để biết `pixelFormat` và `size` thực tế — dùng thông tin đó cho ffplay/ffmpeg.
- Nếu ffplay không hiện video: chắc chắn dùng đúng `-pixel_format` (nv12 / yuv420p) và `-video_size`.

Kết luận ngắn:
- `src/camera.cpp` sử dụng libcamera để capture frames, cấp buffer dmabuf, mmap các fd, copy plane data vào job queue và write ra STDOUT. Mục tiêu là pipe stream thô sang ffmpeg/ffplay để hiển thị/encode.
- Nếu bạn muốn, tôi có thể:
    - dịch và chú thích từng hàm bằng tiếng Việt (comment inline).
    - vạch ra các sửa cụ thể (ví dụ cải thiện mmap với page alignment và kiểm tra base != nullptr) và cung cấp bản patch (chỉ là gợi ý — theo quy tắc hiện tại tôi không thể chỉnh file trực tiếp mà sẽ đưa code sửa bạn có thể dán).
    - hoặc hướng dẫn cách chạy chi tiết với `CMake` + các lệnh `ffplay`/`ffmpeg`.# camera_realtime
