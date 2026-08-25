cmake_minimum_required(VERSION 3.20)
project(castmirror_app CXX)

set(CMAKE_CXX_STANDARD 20)

add_executable(castmirror cli/main.cc)
target_link_libraries(castmirror PRIVATE castcore)
