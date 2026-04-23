# src/streamer/CMakeLists.txt
project(ffmpeg_streamer)

set(STREAMER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/streamer.cpp
)

add_library(ffmpeg_streamer STATIC ${STREAMER_SOURCES})

target_include_directories(ffmpeg_streamer
        PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Link ffmpeg libs (pkg_check_modules done in src/CMakeLists)
if(DEFINED AVFORMAT_FOUND AND AVFORMAT_FOUND)
    target_include_directories(ffmpeg_streamer PUBLIC ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
    target_link_libraries(ffmpeg_streamer PUBLIC ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES})
endif()

# install (optional)
install(TARGETS ffmpeg_streamer ARCHIVE DESTINATION lib)