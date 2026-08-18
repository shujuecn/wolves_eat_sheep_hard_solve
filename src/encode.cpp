#include "encode.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace wolves {

// ============================================================
// 二项式系数表
// ============================================================

std::array<std::array<uint32_t, 26>, 26> BINOM{};

void init_binom() {
    for (int n = 0; n <= 25; ++n) {
        BINOM[n][0] = 1;
        BINOM[n][n] = 1;
        for (int k = 1; k < n; ++k) {
            BINOM[n][k] = BINOM[n-1][k-1] + BINOM[n-1][k];
        }
    }
}

// ============================================================
// 组合编码核心
// ============================================================

uint32_t encode_combination(const int* sorted_pos, int k) {
    // sorted_pos[0..k-1] 是升序排列的位置（0-indexed）
    // rank = sum_{i=0}^{k-1} C(sorted_pos[i], i+1)
    uint32_t rank = 0;
    for (int i = 0; i < k; ++i) {
        rank += BINOM[sorted_pos[i]][i + 1];
    }
    return rank;
}

void decode_combination(uint32_t rank, int n, int k, int* out) {
    // 从 rank 解码出 k 个位置（升序）
    int pos = n - 1;
    for (int i = k; i >= 1; --i) {
        while (pos >= i - 1 && BINOM[pos][i] > rank) {
            --pos;
        }
        out[i - 1] = pos;
        rank -= BINOM[pos][i];
        --pos;
    }
}

// ============================================================
// 狼编码
// ============================================================

uint32_t encode_wolf_sorted(int p0, int p1, int p2) {
    // p0 < p1 < p2
    return BINOM[p0][1] + BINOM[p1][2] + BINOM[p2][3];
}

uint32_t encode_wolf_bb(uint32_t wolf_bb) {
    int pos[3];
    int cnt = 0;
    uint32_t bb = wolf_bb;
    while (bb) {
        int idx = std::countr_zero(bb);
        bb &= bb - 1;
        pos[cnt++] = idx;
    }
    if (cnt != 3) {
        throw std::runtime_error("encode_wolf_bb: expected exactly 3 wolves");
    }
    // pos 已按升序（因为 countr_zero 从低位到高位）
    return encode_wolf_sorted(pos[0], pos[1], pos[2]);
}

std::array<int, 3> decode_wolf(uint32_t rank) {
    std::array<int, 3> out{};
    decode_combination(rank, 25, 3, out.data());
    return out;
}

// ============================================================
// 羊编码
// ============================================================

uint32_t encode_sheep(const std::array<int, 22>& free_list,
                      const std::vector<int>& sheep_positions, int k) {
    if (k == 0) return 0;

    // 将羊的全局坐标映射到 free_list 中的索引
    std::vector<int> mapped;
    mapped.reserve(k);

    // 建立全局坐标 → free_list 索引的映射
    int global_to_free[25];
    std::fill(std::begin(global_to_free), std::end(global_to_free), -1);
    for (int i = 0; i < 22; ++i) {
        global_to_free[free_list[i]] = i;
    }

    for (int sp : sheep_positions) {
        int fi = global_to_free[sp];
        if (fi < 0) {
            throw std::runtime_error("encode_sheep: sheep on wolf cell");
        }
        mapped.push_back(fi);
    }
    std::sort(mapped.begin(), mapped.end());

    return encode_combination(mapped.data(), k);
}

std::vector<int> decode_sheep(const std::array<int, 22>& free_list,
                              uint32_t rank, int k) {
    std::vector<int> result;
    if (k == 0) return result;

    // 解码出在 free_list 中的索引
    std::vector<int> mapped(k);
    decode_combination(rank, 22, k, mapped.data());

    // 映射回全局坐标
    for (int idx : mapped) {
        result.push_back(free_list[idx]);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ============================================================
// 桶索引计算
// ============================================================

uint64_t bucket_size(int k) {
    // C(25,3) * C(22,k) * 2
    uint64_t sheep_combos = (k >= 0 && k <= 22) ? BINOM[22][k] : 0;
    return 2300ULL * sheep_combos * 2;
}

uint64_t state_index(uint32_t wolf_rank, uint32_t sheep_rank,
                     int k, bool turn) {
    uint64_t sheep_combos = BINOM[22][k];
    return (static_cast<uint64_t>(wolf_rank) * sheep_combos +
            sheep_rank) * 2 + (turn ? 1 : 0);
}

uint32_t wolf_rank_from_index(uint64_t idx, int k) {
    uint64_t sheep_combos = BINOM[22][k];
    return static_cast<uint32_t>(idx / (sheep_combos * 2));
}

uint32_t sheep_rank_from_index(uint64_t idx, int k) {
    uint64_t sheep_combos = BINOM[22][k];
    return static_cast<uint32_t>((idx / 2) % sheep_combos);
}

bool turn_from_index(uint64_t idx) {
    return (idx & 1) != 0; // 1 = sheep turn, 0 = wolf turn
}

// ============================================================
// 预计算 WolfInfo 查找表
// ============================================================

std::vector<WolfInfo> WOLF_INFO;

void init_wolf_info() {
    WOLF_INFO.resize(2300);
    for (uint32_t rank = 0; rank < 2300; ++rank) {
        auto pos = decode_wolf(rank);
        WolfInfo& info = WOLF_INFO[rank];
        info.positions = pos;

        // 构建 free_list 和 global_to_free
        std::fill(info.global_to_free.begin(), info.global_to_free.end(), -1);
        int free_idx = 0;
        for (int cell = 0; cell < 25; ++cell) {
            bool is_wolf = false;
            for (int w : pos) {
                if (cell == w) { is_wolf = true; break; }
            }
            if (!is_wolf) {
                info.free_list[free_idx] = cell;
                info.global_to_free[cell] = free_idx;
                ++free_idx;
            }
        }
    }
}

// ============================================================
// 从 State 计算 rank
// ============================================================

uint32_t compute_wolf_rank(const State& s) {
    return encode_wolf_bb(s.wolf_bb);
}

uint32_t compute_sheep_rank(const State& s, int k, const WolfInfo& info) {
    // 提取羊位置
    std::vector<int> sheep_pos;
    uint32_t bb = s.sheep_bb;
    while (bb) {
        int idx = std::countr_zero(bb);
        bb &= bb - 1;
        sheep_pos.push_back(idx);
    }
    return encode_sheep(info.free_list, sheep_pos, k);
}

} // namespace wolves