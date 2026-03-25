#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <iostream>
#include <cstring>
#include <fstream>

int main() {
    const char* device = "/dev/video0";
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open " << device << std::endl;
        return 1;
    }

    // Cấu hình format (YUYV, 640x480)
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    ioctl(fd, VIDIOC_S_FMT, &fmt);

    // Request buffers
    struct v4l2_requestbuffers req = {};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    // Query buffer
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);

    // Map buffer
    void* buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (buffer == MAP_FAILED) {
        std::cerr << "Failed to mmap buffer" << std::endl;
        close(fd);
        return 1;
    }

    // Queue buffer
    ioctl(fd, VIDIOC_QBUF, &buf);

    // Start streaming
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    // Dequeue buffer (capture frame)
    ioctl(fd, VIDIOC_DQBUF, &buf);

    // Lưu frame thành file raw
    std::ofstream outfile("frame.yuyv", std::ios::binary);
    outfile.write(static_cast<char*>(buffer), buf.bytesused);
    outfile.close();

    // Stop streaming
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    // Unmap và close
    munmap(buffer, buf.length);
    close(fd);

    std::cout << "Captured frame saved to frame.yuyv" << std::endl;
    return 0;
}