# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(padlink VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets editors (clangd) understand the code
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PADLINK_BUILD_HOSTAPD "Build the patched hostapd (needs git, libnl, OpenSSL)" ON)
option(PADLINK_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- x264
# Our own static x264 build with a small patch that reports where each
# macroblock row ends (needed to cut frames into the GamePad's 5 chunks).
set(PADLINK_X264_PREFIX ${CMAKE_BINARY_DIR}/x264)
file(GLOB PADLINK_X264_PATCHES ${CMAKE_SOURCE_DIR}/third_party/x264/patches/*.patch)
add_custom_command(
    OUTPUT ${PADLINK_X264_PREFIX}/lib/libx264.a ${PADLINK_X264_PREFIX}/include/x264.h
    COMMAND ${CMAKE_SOURCE_DIR}/third_party/x264/build.sh ${CMAKE_BINARY_DIR}/x264-src ${PADLINK_X264_PREFIX}
    DEPENDS ${PADLINK_X264_PATCHES} ${CMAKE_SOURCE_DIR}/third_party/x264/build.sh
    COMMENT "Building patched x264"
    VERBATIM)
add_custom_target(padlink_x264 DEPENDS ${PADLINK_X264_PREFIX}/lib/libx264.a)
add_library(x264_padlink STATIC IMPORTED)
set_target_properties(x264_padlink PROPERTIES IMPORTED_LOCATION ${PADLINK_X264_PREFIX}/lib/libx264.a)
file(MAKE_DIRECTORY ${PADLINK_X264_PREFIX}/include)
target_include_directories(x264_padlink INTERFACE ${PADLINK_X264_PREFIX}/include)
target_link_libraries(x264_padlink INTERFACE m ${CMAKE_DL_LIBS} Threads::Threads)
add_dependencies(x264_padlink padlink_x264)

# ---------------------------------------------------------------- core library
# Everything except main(), so tests can link the same code.
add_library(padlink_core STATIC
    src/protocol/pairing.cpp
    src/protocol/wps_credential.cpp
    src/protocol/dhcp.cpp
    src/protocol/input.cpp
    src/protocol/cmd.cpp
    src/input/uinput_device.cpp
    src/input/virtual_gamepad.cpp
    src/net/drc_sockets.cpp
    src/protocol/vstrm.cpp
    src/video/h264.cpp
    src/video/encoder.cpp
    src/video/frame_source.cpp
    src/video/video_sender.cpp
    src/protocol/astrm.cpp
    src/audio/audio_source.cpp
    src/audio/audio_sender.cpp
    src/api/api_server.cpp
    src/wifi/hostapd_config.cpp
    src/wifi/hostapd.cpp
    src/wifi/dhcp_server.cpp
    src/store/pairing_store.cpp
    src/clock/tsf_clock.cpp
    src/util/process.cpp
)
target_include_directories(padlink_core PUBLIC src include)

# ---------------------------------------------------------------- public API
# libpadlink: what apps link against. No root, no x264, no hostapd.
add_library(padlink STATIC
    src/client/padlink_client.cpp
    src/client/image.cpp
    src/api/wire.cpp
)
target_include_directories(padlink PUBLIC include PRIVATE src)
target_link_libraries(padlink PUBLIC Threads::Threads)
target_compile_options(padlink PRIVATE -Wall -Wextra -Wpedantic)

add_executable(padlink-demo examples/demo.cpp)
target_link_libraries(padlink-demo PRIVATE padlink)
target_compile_options(padlink_core PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(padlink_core PUBLIC x264_padlink Threads::Threads)
target_sources(padlink_core PRIVATE src/api/wire.cpp)
add_dependencies(padlink_core padlink_x264)

# ---------------------------------------------------------------- daemon
add_executable(padlinkd src/daemon/main.cpp)
target_link_libraries(padlinkd PRIVATE padlink_core)

add_executable(padlink-video-selftest tools/video_selftest.cpp)
target_link_libraries(padlink-video-selftest PRIVATE padlink_core)
add_executable(padlink-encode-file tools/encode_file.cpp)
target_link_libraries(padlink-encode-file PRIVATE padlink_core)

# ---------------------------------------------------------------- hostapd
if(PADLINK_BUILD_HOSTAPD)
    file(GLOB PADLINK_HOSTAPD_PATCHES ${CMAKE_SOURCE_DIR}/third_party/hostapd/patches/*.patch)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/padlink-hostapd
        COMMAND ${CMAKE_SOURCE_DIR}/third_party/hostapd/build.sh
                ${CMAKE_BINARY_DIR}/hostapd-src ${CMAKE_BINARY_DIR}
        DEPENDS ${PADLINK_HOSTAPD_PATCHES}
                ${CMAKE_SOURCE_DIR}/third_party/hostapd/build.sh
                ${CMAKE_SOURCE_DIR}/third_party/hostapd/hostapd.config
        COMMENT "Building patched hostapd 2.11"
        VERBATIM)
    add_custom_target(padlink_hostapd ALL DEPENDS ${CMAKE_BINARY_DIR}/padlink-hostapd)
endif()

# ---------------------------------------------------------------- tests
if(PADLINK_BUILD_TESTS)
    enable_testing()
    add_executable(padlink_tests
        tests/test_main.cpp
        tests/test_pairing.cpp
        tests/test_wps_credential.cpp
        tests/test_dhcp.cpp
        tests/test_hostapd_config.cpp
        tests/test_pairing_store.cpp
        tests/test_tsf_clock.cpp
        tests/test_input.cpp
        tests/test_cmd.cpp
        tests/test_virtual_gamepad.cpp
        tests/test_vstrm.cpp
        tests/test_h264.cpp
        tests/test_audio.cpp
        tests/test_api.cpp
    )
    target_include_directories(padlink_tests PRIVATE third_party/doctest)
    target_link_libraries(padlink_tests PRIVATE padlink_core)
    target_sources(padlink_tests PRIVATE src/client/padlink_client.cpp src/client/image.cpp)
    add_test(NAME padlink_tests COMMAND padlink_tests)

    # End-to-end encoder check: FFmpeg must decode what the GamePad would get.
    find_program(FFMPEG ffmpeg)
    if(FFMPEG)
        foreach(variant rows late even fullpel)
            add_test(NAME video_gamepad_bitstream_${variant}
                     COMMAND ${CMAKE_SOURCE_DIR}/tests/video_selftest.sh $<TARGET_FILE:padlink-video-selftest> ${FFMPEG} ${variant})
        endforeach()
    endif()
endif()
