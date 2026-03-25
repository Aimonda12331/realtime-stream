cmake_minimum_required(VERSION 3.10)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Thư viện nội bộ từ Camera.cpp
add_library(camcore STATIC camera.cpp)

target_include_directories(camcore PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
)

# Tránh link libcamera trực tiếp vào target "camera" (có thể sinh ra
# lỗi "target links to itself" nếu tên trùng). Link libcamera vào executable.
# target_link_libraries(camera PUBLIC ${LIBCAMERA_LIBRARIES})

# Executable chính (main.cpp nằm trong src/)
add_executable(bodycam main.cpp)

target_include_directories(bodycam PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
)

target_link_libraries(bodycam PRIVATE
    camcore
    ${LIBCAMERA_LIBRARIES}
    Threads::Threads
    atomic
)