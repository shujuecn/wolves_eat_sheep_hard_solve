#pragma once

#include "board.h"
#include "encode.h"

#include <cstdint>

namespace wolves {

// ============================================================
// 左右镜像对称
// 对称群 = {e, M}，阶 2
// ============================================================

// 镜像一个格子的坐标：c → 4-c
inline constexpr int mirror_cell(int idx) {
    int r = row_of(idx);
    int c = col_of(idx);
    return pos(r, BOARD_SIZE - 1 - c);
}

// 镜像一个位板
uint32_t mirror_bb(uint32_t bb);

// 镜像一个局面
State mirror_state(const State& s);

// 计算规范形（canonical form）：取 min(state, mirror(state))
// 比较基于编码后的 wolf_rank 和 sheep_rank
// 返回 true 表示 state 是规范形（即 state ≤ mirror(state)）
bool is_canonical(const State& s, int k);

// 获取规范形的 wolf_rank 和 sheep_rank
// 如果 state 是规范形，返回其 ranks
// 如果 mirror 是规范形，返回 mirror 的 ranks
void canonical_ranks(const State& s, int k,
                     uint32_t& wolf_rank, uint32_t& sheep_rank);

// 比较两个状态的编码顺序（用于取 min）
// 返回 true 表示 s1 的编码 ≤ s2 的编码
bool encoded_less_or_equal(uint32_t w1, uint32_t s1, int k,
                           uint32_t w2, uint32_t s2);

} // namespace wolves