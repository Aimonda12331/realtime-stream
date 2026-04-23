##root##
cmake_minimum_required(VERSION 3.10)
project(bodyCam 
        VERSION 1.0
        DESCRIPTION "PROJECT BODYCAM USING RASPBERRY PI2W"
        LANGUAGES C CXX 
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optionally allow overriding install prefix
if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "Install prefix")
endif()

# Put compiled binaries under build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add source tree
add_subdirectory(src)



