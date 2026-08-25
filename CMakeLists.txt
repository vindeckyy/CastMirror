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

# Subdirectories
add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(app)
add_subdirectory(tests)
