##root##
cmake_minimum_required(VERSION 3.10)
project(bodyCam 
        VERSION 1.0
        DESCRIPTION "PROJECT BODYCAM USING RASPBERRY PI2W"
        LANGUAGES C CXX 
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#Add subdirs (modules) thêm thư mục con
add_subdirectory(src) #  tạo excutable ở app/



