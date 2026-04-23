#!/bin/bash
# ============================================================================
# BodyCam RTSP Streaming - Automated Setup Script
# Hỗ trợ: Raspberry Pi OS (Bullseye+), Ubuntu 22.04+
# ============================================================================

set -e  # Exit on error

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========== BodyCam Setup Script ==========${NC}"
echo ""

# ============================================================================
# 1. Kiểm tra OS & người dùng
# ============================================================================
echo -e "${YELLOW}[1/6] Kiểm tra môi trường...${NC}"

if [[ $EUID -eq 0 ]]; then
   echo -e "${RED}Lỗi: Script không nên chạy với quyền root!${NC}"
   exit 1
fi

if ! command -v apt &> /dev/null; then
    echo -e "${RED}Lỗi: apt không tìm thấy. Chỉ hỗ trợ Debian/Ubuntu/Raspberry Pi OS${NC}"
    exit 1
fi

echo -e "${GREEN}✓ OS tương thích${NC}"

# ============================================================================
# 2. Cập nhật package list
# ============================================================================
echo ""
echo -e "${YELLOW}[2/6] Cập nhật package list...${NC}"
sudo apt update

# Hỏi nếu cần full-upgrade
read -p "Bạn có muốn chạy 'apt full-upgrade' không? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    sudo apt full-upgrade -y
fi

echo -e "${GREEN}✓ Package list đã cập nhật${NC}"

# ============================================================================
# 3. Cài đặt dependencies
# ============================================================================
echo ""
echo -e "${YELLOW}[3/6] Cài đặt dependencies...${NC}"

PACKAGES=(
    "libcamera-dev"           # libcamera
    "libcamera-tools"         # tools
    "libyaml-cpp-dev"         # yaml-cpp
    "cmake"                   # CMake
    "git"                     # Git
    "build-essential"         # Compiler
    "pkg-config"              # pkg-config
    "libavformat-dev"         # FFmpeg
    "libavcodec-dev"          # FFmpeg
    "libavutil-dev"           # FFmpeg
    "libswscale-dev"          # FFmpeg
)

echo "Cài đặt: ${PACKAGES[@]}"
sudo apt install -y "${PACKAGES[@]}"

echo -e "${GREEN}✓ Dependencies đã cài đặt${NC}"

# ============================================================================
# 4. Kiểm tra cài đặt
# ============================================================================
echo ""
echo -e "${YELLOW}[4/6] Kiểm tra cài đặt...${NC}"

check_command() {
    if command -v $1 &> /dev/null; then
        echo -e "${GREEN}✓ $1 OK${NC}"
        return 0
    else
        echo -e "${RED}✗ $1 NOT FOUND${NC}"
        return 1
    fi
}

check_pkg() {
    if pkg-config --exists "$1" 2>/dev/null; then
        VERSION=$(pkg-config --modversion "$1")
        echo -e "${GREEN}✓ $1 ($VERSION)${NC}"
        return 0
    else
        echo -e "${RED}✗ $1 NOT FOUND${NC}"
        return 1
    fi
}

echo "Checking tools:"
check_command cmake
check_command git
check_command make

echo ""
echo "Checking libraries:"
check_pkg libcamera
check_pkg libavformat
check_pkg libavcodec
check_pkg libavutil
check_pkg libswscale
check_pkg yaml-cpp

echo -e "${GREEN}✓ Kiểm tra xong${NC}"

# ============================================================================
# 5. Clone/Prepare project (nếu chưa có)
# ============================================================================
echo ""
echo -e "${YELLOW}[5/6] Chuẩn bị project...${NC}"

if [ ! -f "CMakeLists.txt" ]; then
    echo -e "${YELLOW}Project chưa được tải. Hãy clone repo trước!${NC}"
    echo "Ví dụ: git clone <url>"
    exit 1
else
    echo -e "${GREEN}✓ Project đã sẵn sàng${NC}"
fi

# ============================================================================
# 6. Build project
# ============================================================================
echo ""
echo -e "${YELLOW}[6/6] Build project...${NC}"

if [ -d "build" ]; then
    read -p "Thư mục 'build' đã tồn tại. Xóa và build lại? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf build
    else
        echo "Sử dụng build directory hiện tại..."
    fi
fi

mkdir -p build
cd build

echo "Chạy CMake..."
cmake ..

echo ""
echo "Biên dịch (parallel jobs)..."
JOBS=$(nproc)
echo "Sử dụng $JOBS CPU cores..."
make -j$JOBS

cd ..

if [ -f "build/src/bodycam" ]; then
    echo -e "${GREEN}✓ Build thành công!${NC}"
    echo -e "${GREEN}Executable: build/src/bodycam${NC}"
else
    echo -e "${RED}✗ Build thất bại!${NC}"
    exit 1
fi

# ============================================================================
# Setup hoàn tất
# ============================================================================
echo ""
echo -e "${GREEN}========== Setup Hoàn Tất ==========${NC}"
echo ""
echo "Các bước tiếp theo:"
echo "1. Chỉnh sửa config.yaml:"
echo "   - Đặt camera.width, camera.height, camera.fps"
echo "   - Đặt stream.url (RTSP server)"
echo ""
echo "2. Chạy app (xem live video):"
echo "   cd build"
echo "   ./src/bodycam | ffplay -f rawvideo -pixel_format yuv420p -video_size 640x480 -framerate 25 -i -"
echo ""
echo "3. Hoặc stream qua RTSP:"
echo "   ./src/bodycam"
echo ""
echo "4. Xem logs chi tiết:"
echo "   grep '\\[' stderr.log"
echo ""
echo -e "${GREEN}Hướng dẫn chi tiết: Xem file SETUP.md${NC}"

