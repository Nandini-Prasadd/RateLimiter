cmake_minimum_required(VERSION 3.20)
project(AdaptiveRateLimiter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX /EHsc)
else()
    add_compile_options(-Wall -Wextra -Werror -O3)
endif()

include(FetchContent)

# Fetch nlohmann/json
message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Fetch cpp-httplib
message(STATUS "Fetching cpp-httplib...")
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# Fetch googletest
message(STATUS "Fetching GoogleTest...")
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Common library for rate limiter core logic
add_library(rate_limiter_lib
    src/core/RateLimiterCore.hpp
    src/storage/IRateLimitStore.hpp
    src/storage/InMemoryStore.hpp
    src/storage/InMemoryStore.cpp
    src/storage/RedisClient.hpp
    src/storage/RedisClient.cpp
    src/storage/RedisStore.hpp
    src/storage/RedisStore.cpp
    src/metrics/SlidingWindowMetrics.hpp
    src/metrics/SlidingWindowMetrics.cpp
    src/metrics/PrometheusExporter.hpp
    src/metrics/PrometheusExporter.cpp
    src/adaptive/AdaptiveController.hpp
    src/adaptive/AdaptiveController.cpp
)

target_include_directories(rate_limiter_lib PUBLIC src)
target_link_libraries(rate_limiter_lib PUBLIC nlohmann_json::nlohmann_json)

# Thread and socket library linking
find_package(Threads REQUIRED)
target_link_libraries(rate_limiter_lib PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(rate_limiter_lib PUBLIC ws2_32)
endif()

# Main Proxy Server executable
add_executable(rate_limiter_proxy
    src/server/ProxyServer.cpp
)
target_link_libraries(rate_limiter_proxy PRIVATE rate_limiter_lib httplib::httplib)

# Backend Simulator executable
add_executable(backend_simulator
    src/backend_simulator/BackendSimulator.cpp
)
target_link_libraries(backend_simulator PRIVATE rate_limiter_lib httplib::httplib)

# Unit & Concurrency Tests executable
add_executable(rate_limiter_tests
    tests/RateLimiterTests.cpp
)
target_link_libraries(rate_limiter_tests PRIVATE rate_limiter_lib GTest::gtest_main GTest::gmock)

# Copy config folder to build output folder for easy local running
add_custom_command(TARGET rate_limiter_proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:rate_limiter_proxy>/config
)
add_custom_command(TARGET rate_limiter_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:rate_limiter_tests>/config
)
