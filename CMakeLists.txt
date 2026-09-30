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

# The GTK desktop application is the Linux UI. Windows ships the WinUI 3 client
# (app/winui) and the CLI instead. GTK4 does exist in an MSYS2 toolchain, so
# leaving this ON there produced a cascade of X11/POSIX header errors instead of
# a decision; app/CMakeLists.txt now rejects an explicit ON with one actionable
# message.
if(WIN32)
  set(CASTMIRROR_GUI_DEFAULT OFF)
else()
  set(CASTMIRROR_GUI_DEFAULT ON)
endif()
option(CASTMIRROR_BUILD_GUI "Build the GTK desktop application (Linux UI)" ${CASTMIRROR_GUI_DEFAULT})
option(CASTMIRROR_ENABLE_TRAY "Build system tray support when available" ON)
# Hard gate for release builds: refuse to produce an artifact that silently
# omits the tray when the packaging job explicitly asked for it. Off by default
# so an ordinary developer machine without the Ayatana dev package still builds.
option(CASTMIRROR_REQUIRE_TRAY "Fail configuration if tray support was requested but the Ayatana AppIndicator library is missing" OFF)

# ---------------------------------------------------------------------------
# CPack packaging: DEB and RPM.
#
# The dependency list is NOT hand-written. CPACK_DEBIAN_PACKAGE_SHLIBDEPS makes
# dpkg-shlibdeps derive it from the binaries that are actually shipped, so a
# distro bumping libavcodec60 -> libavcodec61 (or libswscale7 -> libswscale10,
# which publish/ already carries for the Windows build) can no longer silently
# produce a package with a stale, uninstallable dependency line.
# ---------------------------------------------------------------------------
set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_PACKAGE_NAME "castmirror")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CONTACT "CastMirror Maintainers <maintainers@castmirror.local>")
set(CPACK_PACKAGE_VENDOR "CastMirror")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/vindeckyy/CastMirror")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Chromecast display mirroring for Linux")
set(CPACK_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}. CastMirror sends a display or a single application window to a Google Cast / Android TV receiver over the LAN without Chrome: X11 (XRandR/XComposite/MIT-SHM) or Wayland (xdg-desktop-portal ScreenCast over PipeWire) capture, VAAPI or libx264 H.264 encoding, Opus audio, and the Cast Streaming CAST/CST2 protocol over UDP with compound RTCP.")
set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/docs/assets/logo.svg")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

# --- Debian ----------------------------------------------------------------
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "CastMirror Maintainers <maintainers@castmirror.local>")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/vindeckyy/CastMirror")
set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.deb")

# Resolve Depends from the shipped ELF objects. Everything CastMirror links
# (libprotobuf, libavcodec/libswscale/libavutil, libx264, libopus, libssl,
# libpulse, libpipewire, X11/Xext/XRandR/XFixes/Xdamage/Xcomposite, libglib,
# libgtk-4, libadwaita and - when the tray is built - the Ayatana AppIndicator
# library) is a soname dpkg-shlibdeps maps to the right distro package on the
# build host, so no soname is enumerated here. Only dependencies dpkg-shlibdeps
# cannot see are listed: the portal is a D-Bus service, not a linked library.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-desktop-portal")

if(NOT CPACK_DEBIAN_PACKAGE_ARCHITECTURE)
    find_program(CASTMIRROR_DPKG_EXECUTABLE dpkg)
    if(CASTMIRROR_DPKG_EXECUTABLE)
        execute_process(COMMAND ${CASTMIRROR_DPKG_EXECUTABLE} --print-architecture
                        OUTPUT_VARIABLE CASTMIRROR_DEB_ARCH
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
    endif()
    if(NOT CASTMIRROR_DEB_ARCH)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            set(CASTMIRROR_DEB_ARCH "amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(CASTMIRROR_DEB_ARCH "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
            set(CASTMIRROR_DEB_ARCH "i386")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7.*|armhf)$")
            set(CASTMIRROR_DEB_ARCH "armhf")
        else()
            set(CASTMIRROR_DEB_ARCH "unknown")
        endif()
    endif()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CASTMIRROR_DEB_ARCH}")
endif()

# --- RPM -------------------------------------------------------------------
# The shipped binary links libx264 through FFmpeg, so it is distributed under
# the GPL - see NOTICE: "Distributing a compiled CastMirror binary that is
# linked against GPL-licensed libx264 generally requires treating that binary
# as GPL".
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_URL "https://github.com/vindeckyy/CastMirror")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.rpm")

# --- Changelog -------------------------------------------------------------
# Shipped as <datadir>/doc/castmirror/changelog so a bare .deb or .rpm is
# identifiable without network access.
set(CASTMIRROR_CHANGELOG_FILE "${CMAKE_BINARY_DIR}/CHANGELOG")
set(CASTMIRROR_CHANGELOG_TEXT "castmirror ${PROJECT_VERSION}\n\n")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} log --no-merges --date=short
                            "--pretty=format:%h%x09%ad%x09%s" -n 200
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE CASTMIRROR_GIT_LOG
                    RESULT_VARIABLE CASTMIRROR_GIT_LOG_RESULT
                    ERROR_QUIET)
    if(CASTMIRROR_GIT_LOG_RESULT EQUAL 0 AND CASTMIRROR_GIT_LOG)
        string(APPEND CASTMIRROR_CHANGELOG_TEXT "${CASTMIRROR_GIT_LOG}\n")
    endif()
endif()
file(WRITE "${CASTMIRROR_CHANGELOG_FILE}" "${CASTMIRROR_CHANGELOG_TEXT}")
install(FILES "${CASTMIRROR_CHANGELOG_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/${CPACK_PACKAGE_NAME}"
        RENAME changelog)

# Subdirectories
add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(app)
add_subdirectory(tests)

# The GTK/adwaita and AppIndicator sonames are part of the shipped binaries, so
# dpkg-shlibdeps resolves their packages (CPACK_DEBIAN_PACKAGE_SHLIBDEPS above).
# They are deliberately not appended to CPACK_DEBIAN_PACKAGE_DEPENDS by hand:
# a hand-written package name that a distro renames produces an uninstallable
# .deb, which is the failure mode this block replaces.
if(CASTMIRROR_BUILD_GUI AND CASTMIRROR_ENABLE_TRAY AND CASTMIRROR_REQUIRE_TRAY
   AND NOT CASTMIRROR_TRAY_LINKED)
    message(FATAL_ERROR
        "CASTMIRROR_REQUIRE_TRAY=ON but the Ayatana AppIndicator library was not found. "
        "Install the dev package (Debian/Ubuntu: libayatana-appindicator-glib-dev) or "
        "configure with -DCASTMIRROR_ENABLE_TRAY=OFF to build without a system tray.")
endif()

include(CPack)
