# src/camera/CMakeLists.txt
project(camcore_module)

set(CAM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/camera.cpp
)

add_library(camcore STATIC ${CAM_SOURCES})

target_include_directories(camcore
        PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Link libcamera if found by parent (pkg_check_modules in src/CMakeLists.txt)
# Use pkg-config variables provided there: LIBCAMERA_FOUND, LIBCAMERA_INCLUDE_DIRS, LIBCAMERA_LIBRARIES
if(DEFINED LIBCAMERA_FOUND AND LIBCAMERA_FOUND)
    target_include_directories(camcore PUBLIC ${LIBCAMERA_INCLUDE_DIRS})
    target_link_libraries(camcore PUBLIC ${LIBCAMERA_LIBRARIES})
endif()

# install
install(TARGETS camcore ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION include FILES_MATCHING PATTERN "*.h")