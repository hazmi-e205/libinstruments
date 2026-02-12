# This file exists for standalone tool builds.
# The tool is also built by the parent CMakeLists.txt when INSTRUMENTS_BUILD_TOOL=ON.

cmake_minimum_required(VERSION 3.16)
project(instruments-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# If built standalone, find the library
if(NOT TARGET instruments)
    find_package(instruments REQUIRED)
endif()

add_executable(instruments-cli main.cpp)
target_link_libraries(instruments-cli PRIVATE instruments)

if(WIN32)
    target_link_libraries(instruments-cli PRIVATE ws2_32)
else()
    target_link_libraries(instruments-cli PRIVATE pthread)
endif()

install(TARGETS instruments-cli DESTINATION bin)
