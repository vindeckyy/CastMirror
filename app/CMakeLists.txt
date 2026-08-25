cmake_minimum_required(VERSION 3.20)
project(castmirror_app CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# CLI Binary
add_executable(castmirror cli/main.cc)
target_link_libraries(castmirror PRIVATE castcore)

# Modern Desktop GUI Binary
add_executable(castmirror-gui gui/main.cc)
target_include_directories(castmirror-gui PRIVATE ${GTK3_INCLUDE_DIRS})
target_compile_options(castmirror-gui PRIVATE ${GTK3_CFLAGS_OTHER})
target_link_libraries(castmirror-gui PRIVATE castcore ${GTK3_LIBRARIES})
