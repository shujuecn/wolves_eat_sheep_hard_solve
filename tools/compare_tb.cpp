// compare_tb.cpp: 逐状态对比两个 k 桶（结果码 + 距离），打印差异统计
#include "board.h"
#include "encode.h"
#include "tablebase.h"

#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace wolves;

struct Loaded {
    int fd = -1;
    uint8_t* map = nullptr;
    uint64_t size = 0;
    const uint8_t* data = nullptr;

    bool load(const std::string& path, int k) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        ::fstat(fd, &st);
        uint64_t expect = sizeof(TBHeader) + bucket_size(k);
        if ((uint64_t)st.st_size != expect) {
            std::cerr << "size mismatch: " << st.st_size << " vs " << expect << "\n";
            ::close(fd); fd = -1; return false;
        }
        map = (uint8_t*)::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        data = map + sizeof(TBHeader);
        size = bucket_size(k);
        return true;
    }
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: compare_tb <k> <fileA> <fileB>\n";
        return 1;
    }
    int k = std::stoi(argv[1]);
    init_binom();
    init_wolf_info();

    Loaded A, B;
    if (!A.load(argv[2], k) || !B.load(argv[3], k)) {
        std::cerr << "failed to load tables\n";
        return 1;
    }

    uint64_t total = A.size;
    uint64_t res_mismatch = 0;
    uint64_t dist_mismatch = 0;
    uint64_t res_counts[4] = {0, 0, 0, 0};
    uint64_t res_countsB[4] = {0, 0, 0, 0};
    int64_t dist_diff_sum = 0;
    uint64_t dist_diff_abs = 0;
    uint64_t printed = 0;

    for (uint64_t idx = 0; idx < total; ++idx) {
        uint8_t ra = tb_result(A.data[idx]);
        uint8_t rb = tb_result(B.data[idx]);
        res_counts[ra]++;
        res_countsB[rb]++;
        if (ra != rb) {
            res_mismatch++;
            if (printed < 10) {
                uint32_t wr = wolf_rank_from_index(idx, k);
                uint32_t sr = sheep_rank_from_index(idx, k);
                bool turn = turn_from_index(idx);
                std::cout << "RES mismatch idx=" << idx << " wr=" << wr
                          << " sr=" << sr << " turn=" << (turn ? "sheep" : "wolf")
                          << " A=" << (int)ra << "/" << (int)tb_distance(A.data[idx])
                          << " B=" << (int)rb << "/" << (int)tb_distance(B.data[idx]) << "\n";
                printed++;
            }
        }
        uint8_t da = tb_distance(A.data[idx]);
        uint8_t db = tb_distance(B.data[idx]);
        if (ra != 2 && rb != 2 && ra != 3 && rb != 3 && da != db) {
            dist_mismatch++;
            dist_diff_sum += (int64_t)db - (int64_t)da;
            dist_diff_abs += (uint64_t)std::llabs((int64_t)db - (int64_t)da);
        }
    }

    std::cout << "=== k=" << k << " total=" << total << " ===\n";
    std::cout << "A results: WOLF=" << res_counts[0] << " SHEEP=" << res_counts[1]
              << " DRAW=" << res_counts[2] << " UNKNOWN=" << res_counts[3] << "\n";
    std::cout << "B results: WOLF=" << res_countsB[0] << " SHEEP=" << res_countsB[1]
              << " DRAW=" << res_countsB[2] << " UNKNOWN=" << res_countsB[3] << "\n";
    std::cout << "result mismatches: " << res_mismatch << "\n";
    std::cout << "distance mismatches (non-draw): " << dist_mismatch
              << "  avg signed diff=" << (dist_mismatch ? (double)dist_diff_sum / dist_mismatch : 0)
              << "  avg |diff|=" << (dist_mismatch ? (double)dist_diff_abs / dist_mismatch : 0) << "\n";
    return (res_mismatch == 0) ? 0 : 1;
}