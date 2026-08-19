cmake_minimum_required(VERSION 3.16)
project(wolves_eat_sheep_hard_solve VERSION 1.0 LANGUAGES CXX)

# ============================================================
# 狼羊棋逆向求解器 — 跨平台构建（Windows / macOS / Linux）
#
# 用法：
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j            # 全部工具
#   ./build/solve_retro               # 逆向分析求解器
#
# 常用选项：
#   -DWOLVES_NATIVE_OPT=ON   GCC/Clang 加 -march=native（最快，但二进制只在本机跑）
#   -DWOLVES_ENABLE_LTO=OFF  关闭 LTO（某些平台链接器不兼容时）
#   Windows/MSVC 示例：
#     cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 可调选项 ----
option(WOLVES_ENABLE_LTO "启用链接期优化 (LTO)" ON)
option(WOLVES_NATIVE_OPT  "GCC/Clang 启用 -march=native（本机微架构优化，二进制不可跨机迁移）" OFF)

# ---- 平台相关编译选项 ----
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Linux / macOS / 其他 Unix
    add_compile_options(-Wall -Wextra)
    if(WOLVES_NATIVE_OPT)
        add_compile_options(-march=native)
    endif()
    if(WOLVES_ENABLE_LTO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        # Apple Silicon 微架构（Apple Clang）
        add_compile_options(-mcpu=apple-m1)
    endif()
elseif(MSVC)
    # Windows / MSVC
    add_compile_options(/W4 /permissive-)
    if(WOLVES_ENABLE_LTO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

# std::thread（所有平台）
find_package(Threads REQUIRED)

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
target_link_libraries(encode PUBLIC board)

# symmetry 库
add_library(symmetry STATIC
    src/symmetry.cpp
)
target_include_directories(symmetry PUBLIC include)
target_link_libraries(symmetry PUBLIC board encode)

# tablebase 库（见 include/platform.h：mmap/文件 I/O 已跨平台封装）
add_library(tablebase STATIC
    src/tablebase.cpp
)
target_include_directories(tablebase PUBLIC include)
target_link_libraries(tablebase PUBLIC board encode)

# solver 库（std::thread 并行）
add_library(solver STATIC
    src/solver.cpp
)
target_include_directories(solver PUBLIC include)
target_link_libraries(solver PUBLIC board encode symmetry tablebase Threads::Threads)

# solver_retro 库（逆向分析优化版：无锁原子 + 匿名大块内存，见 platform.h）
add_library(solver_retro STATIC
    src/solver_retro.cpp
)
target_include_directories(solver_retro PUBLIC include)
# solver_retro 复用 solver.cpp 的全局走法表 MOVE_TABLE，须链接 solver
target_link_libraries(solver_retro PUBLIC board encode symmetry tablebase solver Threads::Threads)

# ============================================================
# 工具
# ============================================================

# 走法对拍工具（vs 暴力全排列）
add_executable(crosscheck
    tools/crosscheck.cpp
)
target_link_libraries(crosscheck PRIVATE board)

# 自检工具
add_executable(selfcheck
    tools/selfcheck.cpp
)
target_link_libraries(selfcheck PRIVATE board)

# 经典逆向求解器
add_executable(solve_all
    tools/solve_all.cpp
)
target_link_libraries(solve_all PRIVATE solver)

# 逆向分析求解器（推荐：距离桶传播 + 无锁并行）
add_executable(solve_retro
    tools/solve_retro.cpp
)
target_link_libraries(solve_retro PRIVATE solver solver_retro)

# 开局库导出
add_executable(dump_opening_book
    tools/dump_opening_book.cpp
)
target_link_libraries(dump_opening_book PRIVATE solver)

# 随机抽样验证
add_executable(verify
    tools/verify.cpp
)
target_link_libraries(verify PRIVATE solver)

# 表库自洽性校验（k=4/k=5 逐局面 minimax 独立性校验）
add_executable(check_tb
    tools/check_tb.cpp
)
target_link_libraries(check_tb PRIVATE board encode tablebase Threads::Threads)

# 表库结果分布统计（只读、顺序扫描）
add_executable(stat_tb
    tools/stat_tb.cpp
)
target_link_libraries(stat_tb PRIVATE tablebase)

# ============================================================
# 安装
# ============================================================

install(TARGETS solve_all solve_retro dump_opening_book verify crosscheck
               selfcheck check_tb stat_tb
    RUNTIME DESTINATION bin)

install(DIRECTORY data/ DESTINATION share/wolves_eat_sheep/data
    OPTIONAL)