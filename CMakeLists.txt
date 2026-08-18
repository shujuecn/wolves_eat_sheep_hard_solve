cmake_minimum_required(VERSION 3.16)
project(wolves_eat_sheep_hard_solve VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 优化选项
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -flto")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -fsanitize=address,undefined")

# Apple Silicon 优化
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mcpu=apple-m1")
endif()

# 查找 OpenMP（可选）
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OpenMP_CXX_FLAGS}")
    message(STATUS "OpenMP found")
else()
    message(STATUS "OpenMP not found, using std::thread only")
endif()

# ============================================================
# 核心库
# ============================================================

# board 库
add_library(board STATIC
    src/board.cpp
)
target_include_directories(board PUBLIC include)

# encode 库
add_library(encode STATIC
    src/encode.cpp
)
target_include_directories(encode PUBLIC include)
target_link_libraries(encode board)

# symmetry 库
add_library(symmetry STATIC
    src/symmetry.cpp
)
target_include_directories(symmetry PUBLIC include)
target_link_libraries(symmetry board encode)

# tablebase 库
add_library(tablebase STATIC
    src/tablebase.cpp
)
target_include_directories(tablebase PUBLIC include)
target_link_libraries(tablebase board encode)

# solver 库
add_library(solver STATIC
    src/solver.cpp
)
target_include_directories(solver PUBLIC include)
target_link_libraries(solver board encode symmetry tablebase)

# solver_retro 库（逆向分析优化版）
add_library(solver_retro STATIC
    src/solver_retro.cpp
)
target_include_directories(solver_retro PUBLIC include)
target_link_libraries(solver_retro board encode symmetry tablebase)

# ============================================================
# 工具
# ============================================================

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

# 主求解器
add_executable(solve_all
    tools/solve_all.cpp
)
target_link_libraries(solve_all board encode symmetry tablebase solver)

# 逆向分析求解器
add_executable(solve_retro
    tools/solve_retro.cpp
)
target_link_libraries(solve_retro board encode symmetry tablebase solver solver_retro)

# 开局库导出
add_executable(dump_opening_book
    tools/dump_opening_book.cpp
)
target_link_libraries(dump_opening_book board encode symmetry tablebase solver)

# 验证工具
add_executable(verify
    tools/verify.cpp
)
target_link_libraries(verify board encode symmetry tablebase solver)

# ============================================================
# 安装
# ============================================================

install(TARGETS solve_all solve_retro dump_opening_book verify crosscheck selfcheck
    RUNTIME DESTINATION bin)

# 创建数据目录
install(DIRECTORY data/ DESTINATION share/wolves_eat_sheep/data
    OPTIONAL)