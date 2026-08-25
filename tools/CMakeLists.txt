cmake_minimum_required(VERSION 3.20)
project(castmirror_tools CXX)

set(CMAKE_CXX_STANDARD 20)

add_executable(poc-control poc-control/main.cc)
target_link_libraries(poc-control PRIVATE castcore)

add_executable(poc-streaming poc-streaming/main.cc)
target_link_libraries(poc-streaming PRIVATE castcore)

add_executable(poc-encode poc-encode/main.cc)
target_link_libraries(poc-encode PRIVATE castcore)

add_executable(poc-join poc-join/main.cc)
target_link_libraries(poc-join PRIVATE castcore)

add_executable(fake-receiver fake-receiver/main.cc)
target_link_libraries(fake-receiver PRIVATE castcore)
