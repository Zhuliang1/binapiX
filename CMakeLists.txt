cmake_minimum_required(VERSION 3.2)
project(binapi)

set(CMAKE_CXX_STANDARD 14)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fsanitize=address")

add_definitions(
    -UNDEBUG
    -DDTF_HEADER_ONLY
)

# 添加子目录
add_subdirectory(examples/asynchronous-user_data)
add_subdirectory(examples/asynchronous)
add_subdirectory(examples/synchronous-user_data)
add_subdirectory(examples/synchronous)
add_subdirectory(examples/websockets)
