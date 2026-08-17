cmake_minimum_required(VERSION 3.16)
project(wolves_eat_sheep_hard_solve VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Apple Silicon 优化
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mcpu=apple-m1")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -flto")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -fsanitize=address,undefined")

# 核心库：board
add_library(board STATIC
    src/board.cpp
)
target_include_directories(board PUBLIC include)

# 走法对拍工具
add_executable(crosscheck
    tools/crosscheck.cpp
)
target_link_libraries(crosscheck board)

# 自检工具
add_executable(selfcheck
    tools/selfcheck.cpp
)
target_link_libraries(selfcheck board)