# src/CMakeLists.txt
# Find pkg-config and common libs (we'll also find in modules if necessary)
find_package(PkgConfig REQUIRED)

# Find ffmpeg libs via pkg-config (modules may repeat - ok)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# libcamera via pkg-config
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# yaml-cpp prefer CONFIG package
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    # fallback: try pkg-config name if available on some systems
    pkg_check_modules(YAMLCPP QUIET yaml-cpp)
    if(YAMLCPP_FOUND)
        set(YAMLCPP_INCLUDE_DIRS ${YAMLCPP_INCLUDE_DIRS})
        set(YAMLCPP_LIBRARIES ${YAMLCPP_LIBRARIES})
    endif()
endif()

# Add module subdirs
add_subdirectory(camera)
add_subdirectory(stream)
add_subdirectory(app)