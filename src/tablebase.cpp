#include "tablebase.h"
#include "platform.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

namespace wolves {

// ============================================================
// Tablebase
// ============================================================

Tablebase::~Tablebase() {
    close();
}

Tablebase::Tablebase(Tablebase&& other) noexcept
    : k_(other.k_), data_(other.data_), size_(other.size_),
      path_(std::move(other.path_)), file_backed_(other.file_backed_) {
    other.data_ = nullptr;
    other.k_ = -1;
    other.size_ = 0;
    other.file_backed_ = false;
}

Tablebase& Tablebase::operator=(Tablebase&& other) noexcept {
    if (this != &other) {
        close();
        k_ = other.k_;
        data_ = other.data_;
        size_ = other.size_;
        path_ = std::move(other.path_);
        file_backed_ = other.file_backed_;
        other.data_ = nullptr;
        other.k_ = -1;
        other.size_ = 0;
        other.file_backed_ = false;
    }
    return *this;
}

bool Tablebase::create(const std::string& path, int k) {
    close();

    k_ = k;
    path_ = path;
    size_ = bucket_size(k);

    // 工作区用匿名内存（零填充、页对齐）：求解全程零磁盘 I/O，仅在
    // mark_completed() 时一次性顺序写盘。早期版本把输出文件 MAP_SHARED
    // 当工作内存全程随机写，脏页回写压垮磁盘/RAID5，全部工作线程被内核
    // 写回节流拖进 D 状态（不可中断睡眠）。
    uint64_t file_size = sizeof(TBHeader) + size_;
    uint8_t* map = static_cast<uint8_t*>(os_alloc_zeroed(file_size));
    if (!map) {
        std::cerr << "create: os_alloc_zeroed(" << file_size << ") failed\n";
        data_ = nullptr;
        return false;
    }

    // 写文件头（内存副本；正式文件头在 mark_completed 时随数据一并落盘）
    TBHeader* hdr = reinterpret_cast<TBHeader*>(map);
    std::memset(hdr, 0, sizeof(TBHeader));
    hdr->magic[0] = 'W'; hdr->magic[1] = 'S';
    hdr->magic[2] = 'T'; hdr->magic[3] = 'B';
    hdr->version = 1;
    hdr->k = static_cast<uint8_t>(k);
    hdr->entry_bits = 8;
    hdr->wolf_combos = 2300;
    hdr->sheep_combos = BINOM[22][k];
    hdr->total_entries = size_;
    hdr->completed = 0;

    // 数据指针指向头之后
    data_ = map + sizeof(TBHeader);
    file_backed_ = false;

    return true;
}

bool Tablebase::open(const std::string& path) {
    close();

    // 只读映射已完成表库文件（completed==1 的桶才允许打开；
    // 半成品/不存在的文件返回 false，让上层走 create() 重建）
    uint64_t fsz = 0;
    uint8_t* map = static_cast<uint8_t*>(os_map_file_read(path, fsz));
    if (!map) return false;
    if (fsz < sizeof(TBHeader)) {
        os_unmap(map, fsz);
        return false;
    }

    TBHeader* hdr = reinterpret_cast<TBHeader*>(map);
    if (hdr->magic[0] != 'W' || hdr->magic[1] != 'S' ||
        hdr->magic[2] != 'T' || hdr->magic[3] != 'B' ||
        !hdr->completed) {
        os_unmap(map, fsz);
        return false;
    }

    path_ = path;
    k_ = hdr->k;
    size_ = hdr->total_entries;
    data_ = map + sizeof(TBHeader);
    file_backed_ = true;

    return true;
}

void Tablebase::close() {
    if (data_) {
        uint64_t file_size = sizeof(TBHeader) + size_;
        uint8_t* map_start = data_ - sizeof(TBHeader);
        if (file_backed_) {
            os_unmap(map_start, file_size);  // 只读映射，无回写
        } else {
            os_free(map_start, file_size);   // 匿名工作区
        }
        data_ = nullptr;
    }
    k_ = -1;
    size_ = 0;
    file_backed_ = false;
}

uint8_t Tablebase::get(uint64_t idx) const {
    return data_[idx];
}

uint8_t Tablebase::get_result(uint64_t idx) const {
    return tb_result(data_[idx]);
}

uint8_t Tablebase::get_distance(uint64_t idx) const {
    return tb_distance(data_[idx]);
}

void Tablebase::set(uint64_t idx, uint8_t entry) {
    data_[idx] = entry;
}

void Tablebase::set_result(uint64_t idx, uint8_t result) {
    data_[idx] = (data_[idx] & TB_DIST_MASK) | (result & TB_RESULT_MASK);
}

void Tablebase::set(uint64_t idx, uint8_t result, uint8_t distance) {
    data_[idx] = tb_pack(result, distance);
}

void Tablebase::fill_unknown() {
    std::memset(data_, tb_pack(TB_UNKNOWN, 0), size_);
}

void Tablebase::mark_completed() {
    if (!data_) return;

    // 内存中的文件头标记完成
    TBHeader* hdr = reinterpret_cast<TBHeader*>(data_ - sizeof(TBHeader));
    hdr->completed = 1;

    // 一次性顺序写盘（对 RAID5/机械盘友好）：[头][数据] 从头顺序写。
    // 求解期间数据一直在内存（匿名工作区），这里才产生唯一的磁盘写入。
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "mark_completed: cannot open " << path_ << "\n";
        return;
    }
    const char* p = reinterpret_cast<const char*>(hdr);
    uint64_t remain = sizeof(TBHeader) + size_;
    while (remain > 0) {
        std::streamsize chunk = static_cast<std::streamsize>(
            std::min<uint64_t>(remain, 1u << 20));  // 1MB
        out.write(p, chunk);
        if (!out) {
            std::cerr << "mark_completed: write failed @" << path_ << "\n";
            return;
        }
        p += chunk;
        remain -= static_cast<uint64_t>(chunk);
    }
    out.flush();
}

// ============================================================
// TablebaseManager
// ============================================================

TablebaseManager::TablebaseManager(const std::string& data_dir)
    : data_dir_(data_dir) {
    buckets_.resize(16); // 0..15
}

Tablebase* TablebaseManager::get_bucket(int k) {
    if (k < 0 || k > 15) return nullptr;

    // k < 4 是平凡狼胜桶，不需要表库文件
    if (k < 4) return nullptr;

    // 简单线程安全：如果已打开，直接返回
    if (buckets_[k].is_open()) {
        return &buckets_[k];
    }

    // 尝试打开已有文件（断点续算：已完成桶直接读；半成品/不存在则重建）。
    std::string path = tb_bucket_path(data_dir_, k);
    if (os_file_exists(path) && buckets_[k].open(path)) {
        return &buckets_[k];
    }

    // 创建匿名工作区（文件仅在 mark_completed 时落盘）
    if (buckets_[k].create(path, k)) {
        return &buckets_[k];
    }

    return nullptr;
}

bool TablebaseManager::is_completed(int k) const {
    if (k < 0 || k > 15) return false;
    // k < 4 是平凡狼胜桶
    if (k < 4) return true;
    if (!buckets_[k].is_open()) return false;

    // 读文件头（std::ifstream，跨平台；只读 64 字节）
    std::ifstream in(tb_bucket_path(data_dir_, k), std::ios::binary);
    if (!in) return false;

    TBHeader hdr;
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        return false;
    }
    return hdr.completed == 1;
}

} // namespace wolves