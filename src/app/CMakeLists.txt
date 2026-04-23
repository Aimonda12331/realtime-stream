# src/app/CMakeLists.txt
project(bodycam_app)

set(APP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/app_config.cpp
)

find_package(Threads REQUIRED)

add_executable(bodycam ${APP_SOURCES})

target_include_directories(bodycam PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link internal libs
target_link_libraries(bodycam PRIVATE
        camcore
        ffmpeg_streamer
        ${CMAKE_THREAD_LIBS_INIT}
        atomic
)

# Link yaml-cpp: prefer CMake config package if available
if(yaml-cpp_FOUND)
    target_link_libraries(bodycam PRIVATE yaml-cpp::yaml-cpp)
elseif(YAMLCPP_FOUND)
    target_include_directories(bodycam PRIVATE ${YAMLCPP_INCLUDE_DIRS})
    target_link_libraries(bodycam PRIVATE ${YAMLCPP_LIBRARIES})
endif()

# Install
install(TARGETS bodycam RUNTIME DESTINATION bin)