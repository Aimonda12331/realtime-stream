Trong xử lý **video + âm thanh song song**, nguyên tắc thiết kế chuẩn là:

> **Mỗi luồng dữ liệu (pipeline) có thread riêng**, không dùng một thread xử lý cả hai.

---

## 1. Thiết kế đúng (khuyến nghị)

### Mô hình phổ biến

```
Thread Video:
  - Capture / Decode
  - Process (resize, ISP, AI…)
  - Render / Encode

Thread Audio:
  - Capture
  - Process (filter, resample)
  - Play / Encode

Thread Sync (hoặc main):
  - Đồng bộ A/V (PTS)
  - Điều phối buffer
```

### Lý do

* Video và audio có:

  * **Tần suất khác nhau** (video 25–60 fps, audio 48 kHz)
  * **Độ trễ xử lý khác nhau**
* Tránh:

  * Audio bị giật khi video nặng
  * Video drop frame vì audio block

---

## 2. Không nên: 1 thread xử lý cả video + audio

```
Thread:
  xử lý video
  xử lý audio
  sleep / wait
```

### Vấn đề

* Blocking I/O (read video) → audio bị trễ
* Video decode chậm → audio underrun
* Khó đảm bảo realtime
* Không scale được khi video nặng (AI, OpenGL)

Chỉ chấp nhận khi:

* Demo nhỏ
* Không realtime
* Không có playback

---

## 3. Mô hình chuẩn trong multimedia system

### Producer – Consumer với queue

```
[Video Capture] → (queue) → [Video Process/Render]
[Audio Capture] → (queue) → [Audio Process/Play]
```

* Mỗi queue thread-safe
* Dùng timestamp (PTS) để sync

---

## 4. Đồng bộ audio – video (AV sync)

**Không sync bằng sleep()**

Cách đúng:

* Audio làm **clock master**
* Video render theo PTS audio

```
if (video_pts > audio_clock)
    wait
else
    render frame
```

---

## 5. Mapping với công nghệ thực tế

### GStreamer

* Mỗi element có thread riêng
* Queue element tách thread
* Clock nội bộ tự sync

### FFmpeg

* Decode audio/video thread riêng
* av_sync theo PTS

### ALSA + V4L2 (tự viết)

* Thread video: V4L2 + GPU
* Thread audio: ALSA capture/playback
* Shared clock + ring buffer

---

## 6. Embedded / SoC (Rockchip, RPi, Yocto)

Khuyến nghị:

* **Video thread**: decode → DMABUF → OpenGL / DRM
* **Audio thread**: ALSA → playback
* **Main thread**: control + sync

Tránh:

* mutex lock dài giữa audio và video
* malloc/free trong loop realtime

---

## 7. Kết luận ngắn gọn

* **NÊN**:
  ✔ Thread video riêng
  ✔ Thread audio riêng
  ✔ Đồng bộ bằng timestamp

* **KHÔNG NÊN**:
  ✘ Một thread xử lý cả video và audio

Nếu người dùng muốn, tôi có thể:

* Vẽ sơ đồ thread + queue
* Ví dụ C++ (pthread / std::thread)
* Ví dụ ALSA + V4L2 sync
* Hoặc mapping trực tiếp sang GStreamer pipeline
