cmake_minimum_required(VERSION 3.16)
project(av_bridge CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(STATIC_LINK "Statically link" ON)
option(BUILD_TESTS "Build tests" OFF)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

add_library(thirdparty INTERFACE)
target_include_directories(thirdparty INTERFACE third_party)

add_library(bridgelib STATIC
    src/frame.cpp
    src/media.cpp
    src/pcap.cpp
    src/stream_hub.cpp)
target_include_directories(bridgelib PUBLIC src third_party)
target_link_libraries(bridgelib PUBLIC mbedcrypto thirdparty)
find_package(Threads REQUIRED)
target_link_libraries(bridgelib PUBLIC Threads::Threads)

# 64-bit atomics need libatomic on mipsel32 (lock-free check)
include(CheckCXXSourceCompiles)
set(_save "${CMAKE_REQUIRED_LIBRARIES}")
check_cxx_source_compiles("#include <atomic>\n#include <cstdint>\nstd::atomic<uint64_t> a{0};\nint main(){return (int)a.fetch_add(1);}" HAVE_LOCKFREE_ATOMIC64)
set(CMAKE_REQUIRED_LIBRARIES "${_save}")
if(NOT HAVE_LOCKFREE_ATOMIC64)
    target_link_libraries(bridgelib PUBLIC atomic)
endif()

add_executable(av-bridge src/main.cpp)
target_link_libraries(av-bridge PRIVATE bridgelib)
if(STATIC_LINK)
    target_link_options(av-bridge PRIVATE -static)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(av_tests tests/frame_test.cpp tests/media_test.cpp tests/hub_test.cpp)
    target_link_libraries(av_tests PRIVATE bridgelib)
    set_target_properties(av_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY tests)
    if(STATIC_LINK)
        target_link_options(av_tests PRIVATE -static)
    endif()
endif()
