/**
 * @file    Camera.h
 * @brief   Module capture & streaming video từ libcamera (Raspberry Pi).
 *
 * Class CameraStreamer đóng gói toàn bộ lifecycle:
 *   init() → configure() → startStreaming() → stop() → release()
 *
 * Dữ liệu raw frame (YUV420/NV12) được ghi ra stdout,
 * phù hợp pipe sang ffmpeg / ffplay.
 */

#ifndef CAMERA_H_
#define CAMERA_H_

#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <cstring>
#include <cerrno>

#include <libcamera/libcamera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/formats.h>

/**
 * @class CameraStreamer
 * @brief Quản lý camera qua libcamera: configure, capture, stream raw frames ra stdout.
 *
 * Sử dụng pre-mmap dmabuf + worker thread + queue để đạt throughput cao.
 * Không cho phép copy/move vì quản lý tài nguyên phần cứng.
 *
 * @note Destructor tự gọi release(), đảm bảo cleanup dù quên gọi tường minh.
 *
 * Ví dụ sử dụng:
 * @code
 *   CameraStreamer cam;
 *   cam.init();
 *   cam.configure(libcamera::formats::YUV420, {640, 480});
 *   cam.startStreaming();   // block đến khi stop()
 *   cam.release();
 * @endcode
 */
class CameraStreamer {
public:
    CameraStreamer();
    ~CameraStreamer();

    // Khong cho phep viec move/hoac cope nhe :)
    CameraStreamer(const CameraStreamer &) = delete;
    CameraStreamer &operator=(const CameraStreamer &) = delete;

    // ========================= Vong doi 1 kiep nguoi =========================

    /**
     * @brief Khởi tạo CameraManager, phát hiện và acquire camera đầu tiên.
     * @return true  nếu acquire thành công.
     * @return false nếu không tìm thấy camera hoặc acquire thất bại.
     */
    bool init();

    /**
     * @brief Tạo cấu hình VideoRecording và áp dụng pixel format + kích thước.
     *
     * Thử @p fmt trước; nếu camera không hỗ trợ (Invalid) sẽ fallback sang NV12.
     *
     * @param fmt  Pixel format mong muốn (mặc định YUV420 / I420).
     * @param size Kích thước khung hình (mặc định 640×480).
     * @return 0 thành công, < 0 thất bại.
     */
    int configure(libcamera::PixelFormat fmt = libcamera::formats::YUV420,
                  libcamera::Size size = libcamera::Size(640, 480));
    
    /**
     * @brief Tạo cấu hình VideoRecording và áp dụng pixel format + kích thước.
     *
     * Thử @p fmt trước; nếu camera không hỗ trợ (Invalid) sẽ fallback sang NV12.
     *
     * @param fmt  Pixel format mong muốn (mặc định YUV420 / I420).
     * @param size Kích thước khung hình (mặc định 640×480).
     * @return 0 thành công, < 0 thất bại.
     */
    int startStreaming();   //Bolock cho đến khi stop() được kiu

    /**
     * @brief Yêu cầu dừng streaming (thread-safe, async-signal-safe).
     *
     * Có thể gọi từ signal handler. startStreaming() sẽ thoát vòng lặp
     * và dọn dẹp tài nguyên trước khi return.
     */
    void stop();

    /**
     * @brief Giải phóng toàn bộ tài nguyên: stop camera, release, stop CameraManager.
     *
     * An toàn khi gọi nhiều lần. Destructor cũng tự gọi hàm này.
     */
    void release();

    /**
     * @brief Set RTSP output URL and optionally fps. If set, CameraStreamer will
     *        attempt to open the internal FFmpegStreamer and publish frames to RTSP.
     * @param url RTSP/RTSPS URL (empty to disable)
     * @param fps target frames per second for encoder
     */
    void setRtsp(const std::string &url, int fps = 25) { rtsp_url_ = url; out_fps_ = fps; target_fps_ = fps; }


    // ========================= Query: truy vấn dữ lịu nhó =========================

    /**
     * @brief Kiểm tra camera có đang streaming hay không.
     * @return true nếu đang chạy.
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Lấy tổng số frame đã capture được.
     * @return Số frame (relaxed ordering, dùng cho monitoring).
     */    
    uint64_t frameCount() const { return frameCount_.load(std::memory_order_relaxed); }

    void setFlip(bool hflip, bool vflip) { hflip_ = hflip; vflip_ = vflip; }

    void setTargetFps(int fps) { target_fps_ = fps; }  // ✅ setter
private:
    // ========================= Du lieu noi bo =========================

    /** @brief Entry trong bảng pre-mmap: ánh xạ dmabuf fd → vùng nhớ. */
    struct MmapEntry {
        uint8_t *base = nullptr;    ///< Con trỏ tới vùng mmap, nullptr nếu chưa map.
        size_t  size  = 0;          ///< Kích thước vùng đã mmap (bytes).
    };

    /** @brief Job chứa bản copy dữ liệu 1 frame (Y+U+V hoặc Y+UV). */
    struct FrameJob {
        std::vector<uint8_t> data;  ///< Raw pixel data đã copy từ dmabuf.
    };
    
    // ========================= Internal helpers =========================

    /**
     * @brief Áp dụng pixel format và size vào CameraConfiguration, validate.
     * @param config Con trỏ tới CameraConfiguration đã tạo.
     * @param fmt    Pixel format mong muốn.
     * @param size   Kích thước khung hình.
     * @return 0 thành công, < 0 thất bại.
     */
    int applyPixelFormat(libcamera::CameraConfiguration *config,
                         libcamera::PixelFormat fmt, libcamera::Size size);
    
    /**
     * @brief Pre-mmap tất cả dmabuf fd của các buffer (gọi 1 lần sau allocate).
     * @param buffers Danh sách FrameBuffer đã cấp phát.
     */
    void premapBuffers(const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers);

    /**
     * @brief Giải phóng (munmap) tất cả entry trong bảng mmaps_.
     */
    void cleanupMmaps();

    /**
     * @brief In thông tin planes của từng buffer ra stderr (debug).
     * @param buffers Danh sách FrameBuffer cần in.
     */
    void printPlaneInfo(const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers );
    
    /**
     * @brief Callback khi 1 request hoàn thành: copy frame → requeue → push queue.
     * @param request Request đã hoàn thành từ libcamera.
     */
    void onRequestComplete(libcamera::Request *request);

    /**
     * @brief Ghi đầy đủ @p size bytes ra file descriptor (vòng lặp write).
     * @param fd   File descriptor đích (thường STDOUT_FILENO).
     * @param buf  Con trỏ dữ liệu.
     * @param size Số bytes cần ghi.
     * @return true nếu ghi hết, false nếu lỗi.
     */
    static bool writeAll(int fd, const void *buf, size_t size);

    // ========================= State biến trạng thái =========================
    libcamera::CameraManager           cm_;                 ///< Quản lý danh sách camera.
    std::shared_ptr<libcamera::Camera> camera_;             ///< Camera đang sử dụng.
    libcamera::Stream                  *stream_ = nullptr;  ///< Stream đã configure.

    std::atomic<bool>     running_{false};       ///< Cờ streaming đang chạy.
    bool                  cmStarted_     = false; ///< CameraManager đã start().
    bool                  cameraStarted_ = false; ///< Camera đã start() (đang streaming).

    //pre-mmap table
    std::map<int, MmapEntry> mmaps_;              ///< Bảng pre-mmap: fd → {base, size}.

    // configured format/size (set in configure()) so we can open FFmpegStreamer
    libcamera::PixelFormat configured_pixfmt_{};
    libcamera::Size        configured_size_{};

    // FFmpeg RTSP output (optional)
    std::unique_ptr<class FFmpegStreamer> ff_ = nullptr;
    std::string rtsp_url_;
    int out_fps_ = 25;

    // Frame queue (dùng trong streaming)
    size_t max_job_queue_size_{4};
    std::queue<FrameJob>    jobQueue_;            ///< Hàng đợi frame cho worker thread.
    std::mutex              queueMutex_;          ///< Mutex bảo vệ jobQueue_.
    std::condition_variable queueCv_;             ///< CV thông báo job mới.
    std::atomic<bool>       workerAlive_{false};  ///< Cờ worker thread còn hoạt động.
    std::atomic<uint64_t>   frameCount_{0};       ///< Bộ đếm frame (atomic, relaxed).
    std::atomic<uint64_t> dropped_jobs_{0};

    bool hflip_ = false;
    bool vflip_ = false;

    int target_fps_ = 25;  // thêm này để khóa fps

    // Buffer pool để tái sử dụng
    std::queue<std::vector<uint8_t>> buffer_pool_;
    std::mutex pool_mutex_;
    static const size_t POOL_SIZE = 4;  // pre-allocate 4 buffers

    std::vector<uint8_t> acquireBuffer(size_t size);
    void releaseBuffer(std::vector<uint8_t> &&buf);
};

#endif //CAMERA_H_