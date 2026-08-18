// 诊断3：对比 retro 的狼移动/吃子检测与 board.h gen_moves，寻找不一致
#include "board.h"
#include "encode.h"
#include "solver_retro.h"
#include "solver.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

using namespace wolves;

int main() {
    init_binom();
    init_wolf_info();
    RETRO_MOVE_TABLE.build();

    const int k = 4;
    uint64_t sheep_combos = BINOM[22][k];
    int mismatches = 0;

    for (uint32_t wr = 0; wr < 2300 && mismatches < 20; ++wr) {
        const auto& info = WOLF_INFO[wr];
        for (uint32_t sr = 0; sr < sheep_combos && mismatches < 20; ++sr) {
            // board.h 局面
            State s;
            s.turn = true;  // wolf turn
            for (int wp : info.positions) s.wolf_bb |= (1u << wp);
            auto sheep = decode_sheep(info.free_list, sr, k);
            for (int sp : sheep) s.sheep_bb |= (1u << sp);

            // board.h 的狼后继（捕获集合 + 普通移动集合）
            auto moves = gen_moves(s);
            std::set<std::pair<int,int>> board_moves;  // (from,to)
            std::set<std::pair<int,int>> board_caps;
            for (auto& m : moves) {
                board_moves.insert({m.from, m.to});
                if (m.captured >= 0) board_caps.insert({m.from, m.to});
            }

            // retro 的狼移动
            int sheep_fi[22];
            decode_combination(sr, 22, k, sheep_fi);
            int fi_to_si[22];
            std::fill(fi_to_si, fi_to_si+22, -1);
            for (int i = 0; i < k; ++i) fi_to_si[sheep_fi[i]] = i;

            std::set<std::pair<int,int>> retro_moves;
            std::set<std::pair<int,int>> retro_caps;
            for (auto& m : RETRO_MOVE_TABLE.wolf_moves[wr]) {
                if (m.jumped_pos < 0) {
                    int to_fi = info.global_to_free[m.to_pos];
                    if (to_fi < 0 || fi_to_si[to_fi] >= 0) continue;
                    retro_moves.insert({m.from_pos, m.to_pos});
                } else {
                    int prey_fi = info.global_to_free[m.to_pos];
                    int jumped_fi = info.global_to_free[m.jumped_pos];
                    if (prey_fi < 0 || jumped_fi < 0) continue;
                    if (fi_to_si[prey_fi] < 0) continue;
                    if (fi_to_si[jumped_fi] >= 0) continue;
                    retro_caps.insert({m.from_pos, m.to_pos});
                }
            }

            std::set<std::pair<int,int>> retro_all = retro_moves;
            retro_all.insert(retro_caps.begin(), retro_caps.end());
            if (board_moves != retro_all || board_caps != retro_caps) {
                mismatches++;
                std::cout << "MISMATCH wr=" << wr << " sr=" << sr << "\n";
                std::cout << to_string(s);
                std::cout << "  board_moves:";
                for (auto& p : board_moves) std::cout << " " << p.first << "->" << p.second;
                std::cout << "\n  retro_moves:";
                for (auto& p : retro_moves) std::cout << " " << p.first << "->" << p.second;
                std::cout << "\n  board_caps:";
                for (auto& p : board_caps) std::cout << " " << p.first << "->" << p.second;
                std::cout << "\n  retro_caps:";
                for (auto& p : retro_caps) std::cout << " " << p.first << "->" << p.second;
                std::cout << "\n";
            }
        }
    }
    std::cout << "total mismatches: " << mismatches << "\n";
    return 0;
}
