#include "symmetry.h"

#include <bit>

namespace wolves {

uint32_t mirror_bb(uint32_t bb) {
    uint32_t result = 0;
    uint32_t b = bb;
    while (b) {
        int idx = std::countr_zero(b);
        b &= b - 1;
        result |= (1u << mirror_cell(idx));
    }
    return result;
}

State mirror_state(const State& s) {
    State ms;
    ms.sheep_bb = mirror_bb(s.sheep_bb);
    ms.wolf_bb = mirror_bb(s.wolf_bb);
    ms.turn = s.turn;
    ms.move_count = s.move_count;
    return ms;
}

bool is_canonical(const State& s, int k) {
    uint32_t w_rank = compute_wolf_rank(s);
    const auto& info = WOLF_INFO[w_rank];
    uint32_t s_rank = compute_sheep_rank(s, k, info);

    State ms = mirror_state(s);
    uint32_t mw_rank = compute_wolf_rank(ms);
    const auto& minfo = WOLF_INFO[mw_rank];
    uint32_t ms_rank = compute_sheep_rank(ms, k, minfo);

    return encoded_less_or_equal(w_rank, s_rank, k, mw_rank, ms_rank);
}

void canonical_ranks(const State& s, int k,
                     uint32_t& wolf_rank, uint32_t& sheep_rank) {
    uint32_t w_rank = compute_wolf_rank(s);
    const auto& info = WOLF_INFO[w_rank];
    uint32_t s_rank = compute_sheep_rank(s, k, info);

    State ms = mirror_state(s);
    uint32_t mw_rank = compute_wolf_rank(ms);
    const auto& minfo = WOLF_INFO[mw_rank];
    uint32_t ms_rank = compute_sheep_rank(ms, k, minfo);

    if (encoded_less_or_equal(w_rank, s_rank, k, mw_rank, ms_rank)) {
        wolf_rank = w_rank;
        sheep_rank = s_rank;
    } else {
        wolf_rank = mw_rank;
        sheep_rank = ms_rank;
    }
}

bool encoded_less_or_equal(uint32_t w1, uint32_t s1, int k,
                           uint32_t w2, uint32_t s2) {
    // 先比较 wolf_rank，再比较 sheep_rank
    if (w1 != w2) return w1 < w2;
    return s1 <= s2;
}

} // namespace wolves