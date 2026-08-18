#include "tablebase.h"

#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wolves {

// ============================================================
// Tablebase
// ============================================================

Tablebase::~Tablebase() {
    close();
}

Tablebase::Tablebase(Tablebase&& other) noexcept
    : k_(other.k_), data_(other.data_), size_(other.size_),
      fd_(other.fd_), path_(std::move(other.path_)) {
    other.data_ = nullptr;
    other.fd_ = -1;
    other.k_ = -1;
    other.size_ = 0;
}

Tablebase& Tablebase::operator=(Tablebase&& other) noexcept {
    if (this != &other) {
        close();
        k_ = other.k_;
        data_ = other.data_;
        size_ = other.size_;
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.data_ = nullptr;
        other.fd_ = -1;
        other.k_ = -1;
        other.size_ = 0;
    }
    return *this;
}

bool Tablebase::create(const std::string& path, int k) {
    close();

    k_ = k;
    path_ = path;
    size_ = bucket_size(k);

    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        perror("open");
        return false;
    }

    // 计算文件大小：头 + 数据
    uint64_t file_size = sizeof(TBHeader) + size_;

    // 扩展文件
    if (::ftruncate(fd_, static_cast<off_t>(file_size)) < 0) {
        perror("ftruncate");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // mmap
    data_ = static_cast<uint8_t*>(::mmap(nullptr, file_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
        perror("mmap");
        ::close(fd_);
        fd_ = -1;
        data_ = nullptr;
        return false;
    }

    // 写文件头
    TBHeader* hdr = reinterpret_cast<TBHeader*>(data_);
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
    data_ = data_ + sizeof(TBHeader);

    return true;
}

bool Tablebase::open(const std::string& path) {
    close();

    path_ = path;

    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_ < 0) {
        perror("open");
        return false;
    }

    // 获取文件大小
    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        perror("fstat");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // mmap 整个文件
    uint8_t* map = static_cast<uint8_t*>(::mmap(nullptr, st.st_size,
                                                  PROT_READ | PROT_WRITE,
                                                  MAP_SHARED, fd_, 0));
    if (map == MAP_FAILED) {
        perror("mmap");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 读文件头
    TBHeader* hdr = reinterpret_cast<TBHeader*>(map);
    if (hdr->magic[0] != 'W' || hdr->magic[1] != 'S' ||
        hdr->magic[2] != 'T' || hdr->magic[3] != 'B') {
        ::munmap(map, st.st_size);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    k_ = hdr->k;
    size_ = hdr->total_entries;
    data_ = map + sizeof(TBHeader);

    return true;
}

void Tablebase::close() {
    if (data_) {
        uint64_t file_size = sizeof(TBHeader) + size_;
        uint8_t* map_start = data_ - sizeof(TBHeader);
        ::msync(map_start, file_size, MS_SYNC);
        ::munmap(map_start, file_size);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    k_ = -1;
    size_ = 0;
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
    if (data_) {
        TBHeader* hdr = reinterpret_cast<TBHeader*>(data_ - sizeof(TBHeader));
        hdr->completed = 1;
        uint64_t file_size = sizeof(TBHeader) + size_;
        uint8_t* map_start = data_ - sizeof(TBHeader);
        ::msync(map_start, file_size, MS_SYNC);
    }
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

    // 尝试打开已有文件（断点续算）。仅在文件确实存在时才走 open，
    // 否则直接创建，避免把“文件尚不存在”误报成错误。
    std::string path = data_dir_ + "/wsf_tb_dtc_k" +
                       (k < 10 ? "0" : "") + std::to_string(k) + ".bin";
    if (::access(path.c_str(), F_OK) == 0) {
        if (buckets_[k].open(path)) {
            return &buckets_[k];
        }
    }

    // 创建新文件
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

    // 读文件头
    std::string path = data_dir_ + "/wsf_tb_dtc_k" +
                       (k < 10 ? "0" : "") + std::to_string(k) + ".bin";

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    TBHeader hdr;
    if (::read(fd, &hdr, sizeof(TBHeader)) != sizeof(TBHeader)) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    return hdr.completed == 1;
}

} // namespace wolves