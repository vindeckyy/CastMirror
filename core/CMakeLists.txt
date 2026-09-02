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
    src/device_auth.cc
    src/cast_channel.cc
    src/mirroring_negotiator.cc
    src/frame_crypto.cc
    src/rtp_packetizer.cc
    src/rtcp_parser.cc
    src/cast_transport.cc
    src/display_capture.cc
    src/display_capture_wayland.cc
    src/audio_capture.cc
    src/gpu_processor.cc
    src/video_encoder.cc
    src/latency_hud.cc
    src/display_capture_wgc.cc
    src/video_encoder_mf.cc
    src/audio_capture_wasapi.cc
    src/c_api.cc
    src/audio_encoder.cc
    src/adaptive_controller.cc
    src/session_recovery.cc
    src/http_fallback_server.cc
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
    # Core Xlib via find_package; extensions via pkg-config (reliable across
    # CMake X11 module versions). Xinerama is dropped: enumeration now uses
    # XRandR, which reports per-monitor geometry and refresh rate.
    find_package(X11 REQUIRED)
    pkg_check_modules(XEXT REQUIRED xext)
    pkg_check_modules(XRANDR REQUIRED xrandr)
    pkg_check_modules(XFIXES REQUIRED xfixes)
    pkg_check_modules(XDAMAGE xdamage)
    if(XDAMAGE_FOUND)
        target_compile_definitions(castcore PUBLIC CASTCORE_HAVE_XDAMAGE=1)
        message(STATUS "Found xdamage: ${XDAMAGE_VERSION} - enabling Damage-aware capture")
    endif()
    pkg_check_modules(XCOMPOSITE xcomposite)
    if(XCOMPOSITE_FOUND)
        target_compile_definitions(castcore PUBLIC CASTCORE_HAVE_XCOMPOSITE=1)
        message(STATUS "Found xcomposite: ${XCOMPOSITE_VERSION} - enabling occluded-window capture")
    endif()
    target_include_directories(castcore PUBLIC
        ${PULSE_INCLUDE_DIRS} ${X11_INCLUDE_DIR}
        ${XEXT_INCLUDE_DIRS} ${XRANDR_INCLUDE_DIRS} ${XFIXES_INCLUDE_DIRS}
        ${XDAMAGE_INCLUDE_DIRS} ${XCOMPOSITE_INCLUDE_DIRS})
    target_link_libraries(castcore PUBLIC
        ${PULSE_LIBRARIES} ${X11_LIBRARIES}
        ${XEXT_LIBRARIES} ${XRANDR_LIBRARIES} ${XFIXES_LIBRARIES}
        ${XDAMAGE_LIBRARIES} ${XCOMPOSITE_LIBRARIES})

    pkg_check_modules(PIPEWIRE libpipewire-0.3 gio-2.0)
    if(PIPEWIRE_FOUND)
        target_compile_definitions(castcore PUBLIC CASTCORE_HAVE_PIPEWIRE=1)
        target_include_directories(castcore PUBLIC ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(castcore PUBLIC ${PIPEWIRE_LIBRARIES})
    endif()

    option(CASTMIRROR_USE_AVAHI "Enable native Avahi client discovery backend" ON)
    if(CASTMIRROR_USE_AVAHI)
        pkg_check_modules(AVAHI avahi-client avahi-common)
        if(AVAHI_FOUND)
            target_compile_definitions(castcore PUBLIC CASTCORE_HAVE_AVAHI=1)
            target_include_directories(castcore PUBLIC ${AVAHI_INCLUDE_DIRS})
            target_link_libraries(castcore PUBLIC ${AVAHI_LIBRARIES})
            message(STATUS "Found avahi-client: ${AVAHI_VERSION} - enabling native Avahi discovery backend")
        endif()
    endif()
endif()

if(WIN32)
    target_link_libraries(castcore PUBLIC d3d11 dxgi mfplat mfuuid ole32)
endif()
