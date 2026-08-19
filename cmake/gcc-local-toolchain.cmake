# ============================================================
# gcc-local-toolchain.cmake — 本开发机的自定义 GCC 11 工具链
#
# 适用：系统未安装 g++ / make（或版本过旧）时的备选方案。
# 用法：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-local-toolchain.cmake
#                  -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#
# 普通 Linux/macOS/Windows 用户不需要此文件：直接用系统编译器即可
# （见 CMakeLists.txt 顶部注释）。
# ============================================================

set(TOOLC /home/agent074/tools/gcc-local/usr)

set(CMAKE_CXX_COMPILER ${TOOLC}/bin/g++)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -idirafter ${TOOLC}/include -idirafter ${TOOLC}/include/x86_64-linux-gnu")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L${TOOLC}/lib/x86_64-linux-gnu -Wl,-rpath,${TOOLC}/lib/x86_64-linux-gnu")