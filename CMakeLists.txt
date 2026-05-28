cmake_minimum_required(VERSION 3.16)
project(rate_limiter)

set(CMAKE_CXX_STANDARD 20)

include_directories(src)

add_executable(server src/server.cpp)

target_link_libraries(server hiredis)
