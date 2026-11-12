cmake_minimum_required(VERSION 3.16)
project(c_net_server VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(cnet_common src/common.c)
target_include_directories(cnet_common PUBLIC include)

add_executable(cnet-server src/server.c)
target_include_directories(cnet-server PRIVATE include)
target_link_libraries(cnet-server PRIVATE cnet_common)

find_package(Threads REQUIRED)
add_executable(cnet-client src/client.c)
target_include_directories(cnet-client PRIVATE include)
target_link_libraries(cnet-client PRIVATE cnet_common Threads::Threads)

install(TARGETS cnet-server cnet-client RUNTIME DESTINATION bin)
