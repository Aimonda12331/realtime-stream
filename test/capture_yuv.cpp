#include <libcamera/camera_manager.h>
#include <libcamera/camera.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/framebuffer.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>
#include <libcamera/formats.h>

#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>

#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <chrono>

/*////////////////////////////////////////*
using namespace libcamera;

struct MappedPlane {
    void *mem = nullptr;
    size_t length = 0;
    int fd = -1;
};

static void write_all(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    while (len) {
        ssize_t w = ::write(STDOUT_FILENO, p, len);
        if (w <= 0) break;
        p += w;
        len -= w;
    }
}

std::atomic<bool> running{true};
void handle_sigint(int){ running = false; }

std::shared_ptr<Camera> open_camera(CameraManager &cm) {
    if (cm.start() != 0) throw std::runtime_error("CameraManager start failed");
    if (cm.cameras().empty()) throw std::runtime_error("No camera");
    return cm.cameras()[0];
}

std::unique_ptr<CameraConfiguration> configure_camera(std::shared_ptr<Camera> cam,
                                                      int width, int height,
                                                      unsigned bufferCount = 4)
{
    auto config = cam->generateConfiguration({ StreamRole::VideoRecording });
    if (!config || config->size() == 0) throw std::runtime_error("Bad config");
    StreamConfiguration &sconf = config->at(0);
    sconf.size.width = width;
    sconf.size.height = height;
    sconf.bufferCount = bufferCount;
    if (cam->configure(config.get()) != 0) throw std::runtime_error("Configure failed");
    return config;
}

void allocate_and_map(FrameBufferAllocator &allocator, Stream *stream,
                      std::vector<std::vector<MappedPlane>> &mapped)
{
    if (allocator.allocate(stream) < 0) throw std::runtime_error("Allocator allocate failed");
    auto &buffers = allocator.buffers(stream);
    mapped.resize(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        const FrameBuffer *fb = buffers[i].get();
        auto planes = fb->planes();
        mapped[i].resize(planes.size());
        for (size_t p = 0; p < planes.size(); ++p) {
            int fd = planes[p].fd.get();
            size_t length = planes[p].length;
            off_t offset = static_cast<off_t>(planes[p].offset);
            void *mem = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, offset);
            if (mem == MAP_FAILED) mapped[i][p] = { nullptr, 0, -1 };
            else mapped[i][p] = { mem, length, fd };
        }
    }
}

std::vector<std::unique_ptr<Request>> make_requests(std::shared_ptr<Camera> cam,
                                                    Stream *stream,
                                                    FrameBufferAllocator &allocator)
{
    std::vector<std::unique_ptr<Request>> requests;
    auto &buffers = allocator.buffers(stream);
    for (auto &fbptr : buffers) {
        auto req = cam->createRequest();
        if (!req) throw std::runtime_error("createRequest failed");
        if (req->addBuffer(stream, fbptr.get()) != 0) throw std::runtime_error("addBuffer failed");
        requests.push_back(std::move(req));
    }
    return requests;
}

struct RequestHandler {
    Camera *cam;
    Stream *stream;
    FrameBufferAllocator *allocator;
    std::vector<std::vector<MappedPlane>> *mapped;
    int width;
    int height;
    bool force_i420;

    void requestComplete(Request *request) {
        if (!request) return;
        if (request->status() != Request::RequestComplete) {
            cam->queueRequest(request);
            return;
        }

        auto &bufs = request->buffers();
        auto it = bufs.find(stream);
        if (it == bufs.end()) { cam->queueRequest(request); return; }
        const FrameBuffer *fb = it->second;

        size_t idx = SIZE_MAX;
        auto &alloc = allocator->buffers(stream);
        for (size_t i = 0; i < alloc.size(); ++i)
            if (alloc[i].get() == fb) { idx = i; break; }
        if (idx == SIZE_MAX) { cam->queueRequest(request); return; }

        auto &planes = (*mapped)[idx];

        // If camera provides 3 planes (Y,U,V) and we don't force conversion, write them directly.
        if (planes.size() == 3 && !force_i420) {
            for (auto &p : planes) if (p.mem && p.length) write_all(p.mem, p.length);
        }
        // If camera provides 2 planes (Y + interleaved UV) and we don't force convert, write NV12
        else if (planes.size() == 2 && !force_i420) {
            if (planes[0].mem && planes[0].length) write_all(planes[0].mem, planes[0].length);
            if (planes[1].mem && planes[1].length) write_all(planes[1].mem, planes[1].length);
        }
        // Otherwise convert NV12 (Y + UV) -> I420 (Y, U, V) and write Y, U, V
        else {
            // convert NV12 (2 planes) -> I420 (Y, U, V) with correct strides
            if (planes.size() == 2 && planes[0].mem && planes[1].mem) {
                int w = width, h = height;
                const uint8_t *ybase = (const uint8_t*)planes[0].mem;
                const uint8_t *uvbase = (const uint8_t*)planes[1].mem;

                int yStride = planes[0].length / h;
                int uvStride = planes[1].length / (h / 2);

                // write Y row-by-row (only width bytes per row)
                for (int row = 0; row < h; ++row) {
                    const uint8_t *yrow = ybase + row * yStride;
                    write_all(yrow, (size_t)w);
                }

                // convert UV -> U and V planes (chroma rows)
                size_t chromaW = w / 2;
                size_t chromaH = h / 2;
                std::vector<uint8_t> U(chromaW * chromaH), V(chromaW * chromaH);

                for (int row = 0; row < chromaH; ++row) {
                    const uint8_t *uvrow = uvbase + row * uvStride;
                    for (int col = 0; col < chromaW; ++col) {
                        size_t idx = row * chromaW + col;
                        U[idx] = uvrow[col * 2 + 0];
                        V[idx] = uvrow[col * 2 + 1];
                    }
                }
                write_all(U.data(), U.size());
                write_all(V.data(), V.size());
            } else if (planes.size() == 3) {
                for (auto &p : planes) if (p.mem && p.length) write_all(p.mem, p.length);
            }
        }

        if (::running.load()) {
        cam->queueRequest(request);
        }
    }
};

int main(int argc, char **argv) {
    int width = 1920, height = 1080;
    bool force_i420 = false;
    if (argc >= 3) { width = atoi(argv[1]); height = atoi(argv[2]); }
    if (argc >= 4 && std::string(argv[3]) == "i420") force_i420 = true;

    signal(SIGINT, handle_sigint);

    try {
        CameraManager cm;
        auto cam = open_camera(cm);
        if (cam->acquire() != 0) throw std::runtime_error("Acquire failed");

        auto config = configure_camera(cam, width, height, 4);
        StreamConfiguration &sconf = config->at(0);
        Stream *stream = sconf.stream();

        FrameBufferAllocator allocator(cam);
        std::vector<std::vector<MappedPlane>> mapped;
        allocate_and_map(allocator, stream, mapped);

        auto requests = make_requests(cam, stream, allocator);

        RequestHandler handler;
        handler.cam = cam.get();
        handler.stream = stream;
        handler.allocator = &allocator;
        handler.mapped = &mapped;
        handler.width = width;
        handler.height = height;
        handler.force_i420 = force_i420;

        cam->requestCompleted.connect(&handler, &RequestHandler::requestComplete);

        if (cam->start() != 0) throw std::runtime_error("cam->start() failed");
        for (auto &r : requests) if (cam->queueRequest(r.get()) != 0) std::cerr << "queueRequest failed\n";

        // Loop until SIGINT; frames are written to stdout by handler.
        while (running) sleep(1);

  // signal to handlers not to re-queue
        ::running = false;

        // disconnect handler so no new callbacks will be called
        cam->requestCompleted.disconnect(&handler, &RequestHandler::requestComplete);

        // stop camera (move to Configured->Stopping->Stopped)
        if (cam->stop() != 0)
            std::cerr << "cam->stop() failed\n";

        // small pause to let in-flight callbacks finish
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // unmap plane memory (before freeing allocator)
        for (auto &vec : mapped)
            for (auto &p : vec)
                if (p.mem && p.length) munmap(p.mem, p.length);

        // free allocator buffers for the stream
        allocator.free(stream);

        // release camera and manager
        cam->release();
        cam.reset();
        cm.stop();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

// Arg parsing: dùng atoi (không phát hiện lỗi/tràn).
// Stride: tính length/height có thể sai; cần lấy stride thực từ plane metadata.
// mmap: thất bại chỉ set NULL, không báo lỗi.
// write_all: không xử lý EINTR/EAGAIN/backpressure; blocking trong callback.
// Format: giả định chỉ NV12/I420 — định dạng khác sẽ sai.
// Vòng đời callback: RequestHandler tạo trên stack → nguy cơ race/use-after-free khi dọn dẹp.

*/

///////////////////////////////////////////////////////////////////////////////////////////////


// // simple_capture_stdout.cpp
// #include <libcamera/camera_manager.h>
// #include <libcamera/camera.h>
// #include <libcamera/framebuffer_allocator.h>
// #include <libcamera/framebuffer.h>
// #include <libcamera/request.h>
// #include <libcamera/stream.h>

// #include <sys/mman.h>
// #include <unistd.h>
// #include <signal.h>

// #include <charconv>
// #include <iostream>
// #include <vector>
// #include <memory>
// #include <atomic>
// #include <cstring>
// #include <stdexcept>

// using namespace libcamera;
// std::atomic<bool> running{true};
// static void handle_sigint(int){ running = false; }

// static bool write_all_fd(int fd, const uint8_t *p, size_t len) {
//     while (len) {
//         ssize_t w = ::write(fd, p, len);
//         if (w > 0) { p += w; len -= (size_t)w; continue; }
//         if (w == 0) return false;
//         if (errno == EINTR) continue;
//         if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
//         return false;
//     }
//     return true;
// }

// int parse_int_or_throw(const char *s) {
//     int v = 0;
//     auto res = std::from_chars(s, s + std::strlen(s), v);
//     if (res.ec != std::errc() || res.ptr == s) throw std::invalid_argument("bad int");
//     return v;
// }

// struct MappedPlane { void *mem = nullptr; size_t length = 0; };

// struct Handler {
//     Camera *cam;
//     Stream *stream;
//     FrameBufferAllocator *allocator;
//     std::vector<std::vector<MappedPlane>> *mapped;
//     int width, height;
//     bool force_i420;

//     void requestComplete(Request *request) {
//         if (!request) return;
//         if (request->status() != Request::RequestComplete) {
//             cam->queueRequest(request);
//             return;
//         }

//         auto &bufs = request->buffers();
//         auto it = bufs.find(stream);
//         if (it == bufs.end()) { cam->queueRequest(request); return; }
//         const FrameBuffer *fb = it->second;

//         size_t idx = SIZE_MAX;
//         auto &alloc = allocator->buffers(stream);
//         for (size_t i = 0; i < alloc.size(); ++i)
//             if (alloc[i].get() == fb) { idx = i; break; }
//         if (idx == SIZE_MAX) { cam->queueRequest(request); return; }

//         auto &planes = (*mapped)[idx];

//         // simple handling: if 2 planes (NV12), convert to I420; if 3 planes, assume I420 and write contiguous planes
//         if (planes.size() == 3 && !force_i420) {
//             for (auto &p : planes) if (p.mem && p.length) write_all_fd(STDOUT_FILENO, (const uint8_t*)p.mem, p.length);
//         } else if (planes.size() == 2 && !force_i420) {
//             // write Y then UV (NV12)
//             if (planes[0].mem && planes[0].length) write_all_fd(STDOUT_FILENO, (const uint8_t*)planes[0].mem, planes[0].length);
//             if (planes[1].mem && planes[1].length) write_all_fd(STDOUT_FILENO, (const uint8_t*)planes[1].mem, planes[1].length);
//         } else {
//             // convert NV12 -> I420 (simple stride assumption: length / rows)
//             if (planes.size() == 2 && planes[0].mem && planes[1].mem) {
//                 const uint8_t *ybase = (const uint8_t*)planes[0].mem;
//                 const uint8_t *uvbase = (const uint8_t*)planes[1].mem;
//                 int yStride = (int)(planes[0].length / height);
//                 int uvStride = (int)(planes[1].length / (height/2));

//                 // write Y rows (width bytes each)
//                 for (int r = 0; r < height; ++r)
//                     write_all_fd(STDOUT_FILENO, ybase + r * yStride, (size_t)width);

//                 // build U and V planes then write
//                 int cw = width / 2, ch = height / 2;
//                 std::vector<uint8_t> U(cw * ch), V(cw * ch);
//                 for (int r = 0; r < ch; ++r) {
//                     const uint8_t *urow = uvbase + r * uvStride;
//                     for (int c = 0; c < cw; ++c) {
//                         U[r*cw + c] = urow[c*2 + 0];
//                         V[r*cw + c] = urow[c*2 + 1];
//                     }
//                 }
//                 write_all_fd(STDOUT_FILENO, U.data(), U.size());
//                 write_all_fd(STDOUT_FILENO, V.data(), V.size());
//             } else if (planes.size() == 3) {
//                 for (auto &p : planes) if (p.mem && p.length) write_all_fd(STDOUT_FILENO, (const uint8_t*)p.mem, p.length);
//             }
//         }

//         if (::running.load()) cam->queueRequest(request);
//     }
// };

// int main(int argc, char **argv) {
//     if (argc < 3) {
//         std::cerr << "Usage: " << argv[0] << " WIDTH HEIGHT [i420]\n";
//         return 1;
//     }
//     int width = parse_int_or_throw(argv[1]);
//     int height = parse_int_or_throw(argv[2]);
//     bool force_i420 = (argc >=4 && std::string(argv[3]) == "i420");

//     signal(SIGINT, handle_sigint);

//     try {
//         CameraManager cm;
//         if (cm.start() != 0) throw std::runtime_error("CameraManager start failed");
//         if (cm.cameras().empty()) throw std::runtime_error("No camera");
//         auto cam = cm.cameras()[0];
//         if (cam->acquire() != 0) throw std::runtime_error("Acquire failed");

//         auto config = cam->generateConfiguration({ StreamRole::VideoRecording });
//         StreamConfiguration &sconf = config->at(0);
//         sconf.size.width = width; sconf.size.height = height; sconf.bufferCount = 4;
//         if (cam->configure(config.get()) != 0) throw std::runtime_error("Configure failed");
//         Stream *stream = sconf.stream();

//         FrameBufferAllocator allocator(cam);
//         if (allocator.allocate(stream) < 0) throw std::runtime_error("Allocator allocate failed");
//         auto &buffers = allocator.buffers(stream);
//         std::vector<std::vector<MappedPlane>> mapped(buffers.size());
//         for (size_t i = 0; i < buffers.size(); ++i) {
//             const FrameBuffer *fb = buffers[i].get();
//             auto planes = fb->planes();
//             mapped[i].resize(planes.size());
//             for (size_t p = 0; p < planes.size(); ++p) {
//                 int fd = planes[p].fd.get();
//                 size_t length = planes[p].length;
//                 off_t offset = static_cast<off_t>(planes[p].offset);
//                 void *mem = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, offset);
//                 if (mem == MAP_FAILED) {
//                     int e = errno;
//                     throw std::runtime_error(std::string("mmap failed: ") + std::strerror(e));
//                 }
//                 mapped[i][p] = { mem, length };
//             }
//         }

//         // create requests
//         std::vector<std::unique_ptr<Request>> requests;
//         for (auto &fb : buffers) {
//             auto req = cam->createRequest();
//             if (!req) throw std::runtime_error("createRequest failed");
//             if (req->addBuffer(stream, fb.get()) != 0) throw std::runtime_error("addBuffer failed");
//             requests.push_back(std::move(req));
//         }

//         // handler (keep alive in main)
//         auto handler = std::make_shared<Handler>();
//         handler->cam = cam.get();
//         handler->stream = stream;
//         handler->allocator = &allocator;
//         handler->mapped = &mapped;
//         handler->width = width; handler->height = height;
//         handler->force_i420 = force_i420;

//         cam->requestCompleted.connect(handler.get(), &Handler::requestComplete);

//         if (cam->start() != 0) throw std::runtime_error("cam->start failed");
//         for (auto &r : requests) if (cam->queueRequest(r.get()) != 0) std::cerr << "queueRequest failed\n";

//         while (running) sleep(1);

//         ::running = false;
//         cam->requestCompleted.disconnect(handler.get(), &Handler::requestComplete);
//         cam->stop();

//         // unmap
//         for (auto &vec : mapped)
//             for (auto &p : vec)
//                 if (p.mem && p.length) munmap(p.mem, p.length);

//         allocator.free(stream);
//         cam->release();
//         cm.stop();
//         return 0;
//     } catch (const std::exception &e) {
//         std::cerr << "Error: " << e.what() << "\n";
//         return 1;
//     }
// }