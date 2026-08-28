cmake_minimum_required(VERSION 3.20)
project(CastMirror VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compiler warnings
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

enable_testing()
include(GNUInstallDirs)

option(CASTMIRROR_BUILD_GUI "Build the GTK desktop application" ON)
option(CASTMIRROR_ENABLE_TRAY "Build system tray support when available" ON)

# CPack DEB packaging
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "castmirror")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "CastMirror Maintainers <maintainers@castmirror.local>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "CastMirror Maintainers <maintainers@castmirror.local>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Chromecast display mirroring for Linux")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libopus0, libpulse0, libssl3 | libssl1.1, libprotobuf32 | libprotobuf23, libavcodec60 | libavcodec-extra60 | libavcodec58 | libavcodec61, libswscale7 | libswscale5 | libswscale6 | libswscale8, libx11-6, libxrandr2, libxfixes3, libxext6, libpipewire-0.3-0, libglib2.0-0, libx264-164 | libx264-163 | libx264-160 | libx264-155 | libx264-165 | libx264-152")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/vindeckyy/CastMirror")

# Subdirectories
add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(app)
add_subdirectory(tests)

if(CASTMIRROR_BUILD_GUI)
    string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libgtk-4-1 (>= 4.12), libadwaita-1-0 (>= 1.5)")
endif()
if(CASTMIRROR_TRAY_LINKED)
    string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libayatana-appindicator-glib2")
endif()

include(CPack)
