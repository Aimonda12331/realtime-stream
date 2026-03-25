//g++ -std=c++17 -O2 1_libcamera.cpp -o 1_libcamera `pkg-config --cflags --libs libcamera` -pthread
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

using namespace libcamera;
using namespace std::chrono_literals;

int main() {
    //code to flow
    printf("Check thu vien Libcamera thanh cong\n");
    return 0;
}