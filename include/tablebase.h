#pragma once

#include "board.h"
#include "encode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wolves {

// ============================================================
// 表库文件格式 (.tb)
// 每条目 1 字节：bits 0-1 = result, bits 2-7 = distance (0-63)
// 距离封顶 63（150 步实际用不到 63 以上，但为了安全保留）
// 对于需要更大距离的情况，使用 UNKNOWN 并在验证时处理
// ============================================================

// 结果编码（2 bits）
constexpr uint8_t TB_WOLF_WIN  = 0;  // 00
constexpr uint8_t TB_SHEEP_WIN = 1;  // 01
constexpr uint8_t TB_DRAW      = 2;  // 10
constexpr uint8_t TB_UNKNOWN   = 3;  // 11

// 距离掩码和偏移
constexpr uint8_t TB_DIST_MASK  = 0xFC;  // bits 2-7
constexpr uint8_t TB_RESULT_MASK = 0x03;  // bits 0-1
constexpr int TB_DIST_SHIFT = 2;

// 打包/解包条目
inline uint8_t tb_pack(uint8_t result, uint8_t distance) {
    return (result & 0x03) | ((distance << TB_DIST_SHIFT) & TB_DIST_MASK);
}
inline uint8_t tb_result(uint8_t entry) {
    return entry & TB_RESULT_MASK;
}
inline uint8_t tb_distance(uint8_t entry) {
    return (entry & TB_DIST_MASK) >> TB_DIST_SHIFT;
}

// 文件头（64 字节）
struct TBHeader {
    char magic[4];           // "WSTB"
    uint8_t version;         // 规则版本
    uint8_t k;               // 羊数
    uint8_t entry_bits;      // 条目位宽（8）
    uint8_t reserved1;
    uint32_t wolf_combos;    // 2300
    uint32_t sheep_combos;   // C(22,k)
    uint64_t total_entries;  // 总条目数
    uint8_t completed;       // 0 = 未完成, 1 = 已完成
    uint8_t reserved2[39];   // 填充到 64 字节
};

static_assert(sizeof(TBHeader) == 64, "TBHeader must be 64 bytes");

// ============================================================
// 表库目录与文件命名（所有工具共用）
// ============================================================

// 默认表库目录（本仓库已解出的 k=4..15 全量表库所在目录）
inline constexpr const char* kDefaultDataDir = "data/ws_tb_dtc_260819";

// 桶文件路径：<data_dir>/dtc_kXX.bin（路径分隔统一用 '/'，Windows 兼容）
inline std::string tb_bucket_path(const std::string& data_dir, int k) {
    std::string name = "dtc_k";
    if (k < 10) name += '0';
    name += std::to_string(k);
    name += ".bin";
    return data_dir + "/" + name;
}

// ============================================================
// 表库类
// ============================================================

class Tablebase {
public:
    // 无成员句柄：create() 用匿名内存工作区；open() 只读映射已完成文件
    Tablebase() : k_(-1), data_(nullptr), size_(0), file_backed_(false) {}
    ~Tablebase();

    // 不可复制
    Tablebase(const Tablebase&) = delete;
    Tablebase& operator=(const Tablebase&) = delete;

    // 可移动
    Tablebase(Tablebase&& other) noexcept;
    Tablebase& operator=(Tablebase&& other) noexcept;

    // 创建新表库文件
    bool create(const std::string& path, int k);

    // 打开已有表库文件（mmap）
    bool open(const std::string& path);

    // 关闭并同步
    void close();

    // 读取条目
    uint8_t get(uint64_t idx) const;
    uint8_t get_result(uint64_t idx) const;
    uint8_t get_distance(uint64_t idx) const;

    // 写入条目
    void set(uint64_t idx, uint8_t entry);
    void set_result(uint64_t idx, uint8_t result);
    void set(uint64_t idx, uint8_t result, uint8_t distance);

    // 批量初始化（全部设为 UNKNOWN）
    void fill_unknown();

    // 属性
    int k() const { return k_; }
    uint64_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }
    const std::string& path() const { return path_; }

    // 直接访问数据指针（用于批量操作）
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

    // 标记完成
    void mark_completed();

private:
    int k_;
    uint8_t* data_;
    uint64_t size_;
    std::string path_;
    bool file_backed_;  // true = 只读映射自已完成文件；false = 匿名工作区（求解中）
};

// ============================================================
// 多桶表库管理器
// ============================================================

class TablebaseManager {
public:
    TablebaseManager(const std::string& data_dir);

    // 获取或创建某个 k 的表库
    Tablebase* get_bucket(int k);

    // 检查某个 k 是否已完成
    bool is_completed(int k) const;

    // 获取数据目录
    const std::string& data_dir() const { return data_dir_; }

private:
    std::string data_dir_;
    std::vector<Tablebase> buckets_;  // 索引 0..15（0-3 不使用）
};

} // namespace wolves