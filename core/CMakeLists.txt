cmake_minimum_required(VERSION 3.20)
project(castcore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Dependencies
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(LIBAV REQUIRED libavcodec libswscale libavutil)

# Protobuf compilation
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/proto/cast_channel.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${PROTO_SRC}")

# Core sources
set(CASTCORE_SOURCES
    ${PROTO_SRCS}
    src/logger.cc
    src/config.cc
    src/state_machine.cc
    src/capability_model.cc
    src/device_discovery.cc
    src/cast_channel.cc
    src/mirroring_negotiator.cc
    src/frame_crypto.cc
    src/rtp_packetizer.cc
    src/rtcp_parser.cc
    src/cast_transport.cc
    src/display_capture.cc
    src/audio_capture.cc
    src/gpu_processor.cc
    src/video_encoder.cc
    src/audio_encoder.cc
    src/adaptive_controller.cc
    src/session_recovery.cc
    src/cast_session.cc
    src/cast_engine.cc
)

add_library(castcore STATIC ${CASTCORE_SOURCES})

target_include_directories(castcore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
    ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${OPUS_INCLUDE_DIRS}
    ${LIBAV_INCLUDE_DIRS}
)

target_link_libraries(castcore PUBLIC
    ${Protobuf_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${OPUS_LIBRARIES}
    ${LIBAV_LIBRARIES}
)

if(UNIX AND NOT APPLE)
    pkg_check_modules(PULSE REQUIRED libpulse libpulse-simple)
    find_package(X11)
    target_include_directories(castcore PUBLIC ${PULSE_INCLUDE_DIRS} ${X11_INCLUDE_DIR})
    target_link_libraries(castcore PUBLIC ${PULSE_LIBRARIES} ${X11_LIBRARIES} Xinerama)
endif()
