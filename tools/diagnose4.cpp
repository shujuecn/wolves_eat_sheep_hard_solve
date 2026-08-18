// 诊断4：单独复现 Phase1/Phase2 init，打印目标状态的 cnt/赋值
#include "board.h"
#include "encode.h"
#include "solver_retro.h"
#include "solver.h"
#include "tablebase.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace wolves;

static inline uint32_t encode_fi_sorted(int* fi, int k) {
    std::sort(fi, fi + k);
    return encode_combination(fi, k);
}

int main() {
    init_binom();
    init_wolf_info();
    RETRO_MOVE_TABLE.build();

    const int k = 4;
    uint64_t sheep_combos = BINOM[22][k];
    uint64_t total = 2ull * 2300 * sheep_combos;
    std::vector<uint8_t> tb(total, tb_pack(TB_UNKNOWN, 0));

    auto get = [&](uint32_t wr, uint32_t sr, bool turn) {
        return tb[state_index(wr, sr, k, turn)];
    };

    // ---- Phase 1: wolf turn ----
    std::cout << "=== Phase 1 (wolf turn) ===\n";
    for (uint32_t wr = 0; wr < 2300; ++wr) {
        const auto& info = WOLF_INFO[wr];
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            int sheep_fi[22];
            decode_combination(sr, 22, k, sheep_fi);
            int fi_to_si[22];
            std::fill(fi_to_si, fi_to_si+22, -1);
            for (int i = 0; i < k; ++i) fi_to_si[sheep_fi[i]] = i;

            uint64_t idx = state_index(wr, sr, k, false);
            bool found_win = false, has_any = false;
            uint8_t cnt = 0;
            for (const WolfMove& m : RETRO_MOVE_TABLE.wolf_moves[wr]) {
                if (m.jumped_pos < 0) {
                    int to_fi = info.global_to_free[m.to_pos];
                    if (to_fi < 0 || fi_to_si[to_fi] >= 0) continue;
                    has_any = true; cnt++;
                } else {
                    int prey_fi = info.global_to_free[m.to_pos];
                    int jumped_fi = info.global_to_free[m.jumped_pos];
                    if (prey_fi < 0 || jumped_fi < 0) continue;
                    if (fi_to_si[prey_fi] < 0) continue;
                    if (fi_to_si[jumped_fi] >= 0) continue;
                    has_any = true;
                    found_win = true;
                }
            }
            if (!has_any) tb[idx] = tb_pack(TB_SHEEP_WIN, 0);
            else if (found_win) tb[idx] = tb_pack(TB_WOLF_WIN, 1);
            else tb[idx] = tb_pack(TB_UNKNOWN, 0);  // pending (cnt recorded separately in real solver)
        }
    }

    auto val_wolf = get(0, 7, false);
    std::cout << "A (wr=0 sr=7 wolf): " << (int)tb_result(val_wolf) << "/" << (int)tb_distance(val_wolf) << "\n";
    auto val_wolf8 = get(0, 8, false);
    std::cout << "wr=0 sr=8 wolf: " << (int)tb_result(val_wolf8) << "/" << (int)tb_distance(val_wolf8) << "\n";

    // ---- Phase 2: sheep turn ----
    std::cout << "=== Phase 2 (sheep turn) ===\n";
    for (uint32_t wr = 0; wr < 2300; ++wr) {
        const auto& info = WOLF_INFO[wr];
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            int sheep_fi[22];
            decode_combination(sr, 22, k, sheep_fi);
            int fi_to_si[22];
            std::fill(fi_to_si, fi_to_si+22, -1);
            for (int i = 0; i < k; ++i) fi_to_si[sheep_fi[i]] = i;

            uint64_t idx = state_index(wr, sr, k, true);

            // terminal check
            bool all_blocked = true;
            for (int wi = 0; wi < 3 && all_blocked; ++wi)
                for (int adj_fi : RETRO_MOVE_TABLE.wolf_adj_free[wr][wi])
                    if (fi_to_si[adj_fi] < 0) { all_blocked = false; break; }
            if (all_blocked) { tb[idx] = tb_pack(TB_SHEEP_WIN, 0); continue; }

            bool found_win = false, has_any = false;
            uint8_t cnt = 0, max_loss = 0;
            int cnt_wolfwin = 0, cnt_sheepwin = 0, cnt_unknown = 0;
            for (int si = 0; si < k; ++si) {
                int from_fi = sheep_fi[si];
                for (int to_fi : RETRO_MOVE_TABLE.sheep_adj[wr][from_fi]) {
                    if (fi_to_si[to_fi] >= 0) continue;
                    int nfi[22];
                    for (int i = 0; i < k; ++i) nfi[i] = sheep_fi[i];
                    nfi[si] = to_fi;
                    uint32_t new_sr = encode_fi_sorted(nfi, k);
                    uint64_t succ = state_index(wr, new_sr, k, false);
                    uint8_t r = tb_result(tb[succ]);
                    uint8_t dd = tb_distance(tb[succ]);
                    has_any = true;
                    if (r == TB_SHEEP_WIN) { found_win = true; cnt_sheepwin++; }
                    else if (r == TB_WOLF_WIN) { max_loss = std::max(max_loss, dd); cnt_wolfwin++; }
                    else { cnt++; cnt_unknown++; }
                }
            }
            if (found_win) tb[idx] = tb_pack(TB_SHEEP_WIN, 1);
            else if (has_any && cnt == 0) tb[idx] = tb_pack(TB_WOLF_WIN, (uint8_t)(max_loss+1));
            else tb[idx] = tb_pack(TB_UNKNOWN, 0);

            if (wr == 0 && (sr == 8 || sr == 7)) {
                std::cout << "wr=0 sr=" << sr << " sheep: cnt=" << (int)cnt
                          << " wolfwin_succ=" << cnt_wolfwin
                          << " sheepwin_succ=" << cnt_sheepwin
                          << " unknown_succ=" << cnt_unknown
                          << " max_loss=" << (int)max_loss
                          << " -> val=" << (int)tb_result(tb[idx]) << "/" << (int)tb_distance(tb[idx]) << "\n";
            }
        }
    }

    auto val_sheep8 = get(0, 8, true);
    std::cout << "B (wr=0 sr=8 sheep): " << (int)tb_result(val_sheep8) << "/" << (int)tb_distance(val_sheep8) << "\n";
    return 0;
}
