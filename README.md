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
