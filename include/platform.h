#pragma once

// ============================================================
// platform.h — 跨平台支撑（Windows / macOS / Linux）
//
// 统一封装两类 POSIX 专属能力，保持业务代码平台无关：
//   1. 大块零填充内存（表库工作区 / 计数器）：匿名 mmap <-> VirtualAlloc
//   2. 只读文件映射（读取已完成表库）：MAP_SHARED 只读 <-> MapViewOfFile
//     以及 uint8 原子操作（__atomic_* <-> MSVC _Interlocked*）
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wolves {

// ============================================================
// 大块匿名内存（页对齐、按需零填充）
// ============================================================

inline void* os_alloc_zeroed(std::size_t size) {
#ifdef _WIN32
    return ::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                          PAGE_READWRITE);
#else
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

inline void os_free(void* p, std::size_t size) {
    if (!p) return;
#ifdef _WIN32
    (void)size;
    ::VirtualFree(p, 0, MEM_RELEASE);
#else
    ::munmap(p, size);
#endif
}

// ============================================================
// 只读文件映射（读取已完成表库）
// ============================================================

// 将整个文件只读映射到内存。成功返回基地址并写出文件字节数；失败返回 nullptr。
inline void* os_map_file_read(const std::string& path, std::uint64_t& out_size) {
#ifdef _WIN32
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz)) {
        ::CloseHandle(h);
        return nullptr;
    }
    HANDLE m = ::CreateFileMappingA(h, nullptr, PAGE_READONLY,
                                    static_cast<DWORD>(sz.QuadPart >> 32),
                                    static_cast<DWORD>(sz.QuadPart & 0xFFFFFFFFu),
                                    nullptr);
    ::CloseHandle(h);  // 映射建立后文件句柄可关闭
    if (!m) return nullptr;
    void* p = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0,
                              static_cast<SIZE_T>(sz.QuadPart));
    ::CloseHandle(m);  // 视图建立后映射句柄可关闭
    if (!p) return nullptr;
    out_size = static_cast<std::uint64_t>(sz.QuadPart);
    return p;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                     PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);  // 映射建立后 fd 可关闭
    if (p == MAP_FAILED) return nullptr;
    out_size = static_cast<std::uint64_t>(st.st_size);
    return p;
#endif
}

inline void os_unmap(void* p, std::uint64_t size) {
    if (!p) return;
#ifdef _WIN32
    (void)size;
    ::UnmapViewOfFile(p);
#else
    ::munmap(p, static_cast<std::size_t>(size));
#endif
}

// ============================================================
// 文件存在性判断
// ============================================================

inline bool os_file_exists(const std::string& path) {
#ifdef _WIN32
    DWORD attr = ::GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    return ::access(path.c_str(), F_OK) == 0;
#endif
}

// ============================================================
// uint8 原子操作（内存序 relaxed，与求解热路径语义一致）
// ============================================================

inline std::uint8_t atomic_load_u8(const std::uint8_t* p) {
#ifdef _MSC_VER
    return *reinterpret_cast<volatile const std::uint8_t*>(p);
#else
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#endif
}

inline bool atomic_cas_u8(std::uint8_t* p, std::uint8_t expected,
                          std::uint8_t desired) {
#ifdef _MSC_VER
    return _InterlockedCompareExchange8(
               reinterpret_cast<volatile char*>(p),
               static_cast<char>(desired),
               static_cast<char>(expected)) == static_cast<char>(expected);
#else
    std::uint8_t e = expected;
    return __atomic_compare_exchange_n(p, &e, desired, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
#endif
}

inline std::uint8_t atomic_fetch_sub_u8(std::uint8_t* p, std::uint8_t v) {
#ifdef _MSC_VER
    return static_cast<std::uint8_t>(_InterlockedExchangeAdd8(
        reinterpret_cast<volatile char*>(p), -static_cast<char>(v)));
#else
    return __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
#endif
}

}  // namespace wolves