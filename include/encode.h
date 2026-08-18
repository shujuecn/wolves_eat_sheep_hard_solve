#pragma once

#include "board.h"

#include <array>
#include <cstdint>
#include <vector>

namespace wolves {

// ============================================================
// 组合编码：狼 C(25,3) = 2300，羊 C(22,k) ≤ 705432
// ============================================================

// 预计算的二项式系数 C[n][k]
extern std::array<std::array<uint32_t, 26>, 26> BINOM;

// 初始化二项式系数表（程序启动时调用一次）
void init_binom();

// ---- 狼编码 (C(25,3) = 2300) ----
// 将 3 个排序位置编码为 0..2299
uint32_t encode_wolf_sorted(int p0, int p1, int p2);
// 从 25-bit 位板编码（提取置位位置）
uint32_t encode_wolf_bb(uint32_t wolf_bb);
// 解码：rank → 3 个位置
std::array<int, 3> decode_wolf(uint32_t rank);

// ---- 羊编码 (C(22,k)) ----
// 给定狼占用的 3 个位置，将 k 个羊位置编码
// free_list: 22 个空闲格（全局坐标），按升序排列
// sheep_positions: k 个羊位置（全局坐标），将排序后编码
uint32_t encode_sheep(const std::array<int, 22>& free_list,
                      const std::vector<int>& sheep_positions, int k);
// 解码：给定 free_list 和 rank，返回 k 个羊位置（全局坐标）
std::vector<int> decode_sheep(const std::array<int, 22>& free_list,
                              uint32_t rank, int k);

// ---- 组合辅助 ----
// 从 n 个元素中选 k 个的编码（位置已排序，0-indexed）
uint32_t encode_combination(const int* sorted_pos, int k);
// 解码组合
void decode_combination(uint32_t rank, int n, int k, int* out);

// ============================================================
// 桶索引计算
// ============================================================

// 每个羊数 k 的表大小
uint64_t bucket_size(int k);

// 计算局面在桶内的线性索引
// 索引 = wolf_rank * C(22,k) * 2 + sheep_rank * 2 + turn
uint64_t state_index(uint32_t wolf_rank, uint32_t sheep_rank,
                     int k, bool turn);

// 从索引解码
uint32_t wolf_rank_from_index(uint64_t idx, int k);
uint32_t sheep_rank_from_index(uint64_t idx, int k);
bool turn_from_index(uint64_t idx);

// ---- 预计算查找表 ----
// 对每个 wolf_rank，预计算 free_list 和 global_to_free 映射
struct WolfInfo {
    std::array<int, 3> positions;        // 3 个狼位置（全局坐标）
    std::array<int, 22> free_list;       // 22 个空闲格（全局坐标，升序）
    std::array<int, 25> global_to_free;  // 全局坐标 → free_list 索引（-1 = 被狼占）
};
extern std::vector<WolfInfo> WOLF_INFO;  // 索引 = wolf_rank

void init_wolf_info();

// 给定 State，计算 wolf_rank 和 sheep_rank
uint32_t compute_wolf_rank(const State& s);
uint32_t compute_sheep_rank(const State& s, int k, const WolfInfo& info);

} // namespace wolves