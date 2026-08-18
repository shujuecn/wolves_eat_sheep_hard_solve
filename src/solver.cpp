#include "solver.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

namespace wolves {

// ============================================================
// MoveTable
// ============================================================

MoveTable::MoveTable() {
    // 初始化所有条目为 -1
    for (int i = 0; i < 25; ++i) {
        for (int d = 0; d < 4; ++d) {
            adjacent[i][d] = -1;
            capture_prey[i][d] = -1;
            capture_jumped[i][d] = -1;
        }
    }

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int from = pos(r, c);

            for (int d = 0; d < 4; ++d) {
                int nr = r + DIRS[d][0];
                int nc = c + DIRS[d][1];
                if (!in_bounds(nr, nc)) continue;

                int adj = pos(nr, nc);
                adjacent[from][d] = adj;

                // 吃子：隔一格
                int pr = r + 2 * DIRS[d][0];
                int pc = c + 2 * DIRS[d][1];
                if (!in_bounds(pr, pc)) continue;

                capture_prey[from][d] = pos(pr, pc);
                capture_jumped[from][d] = adj;
            }
        }
    }
}

const MoveTable MOVE_TABLE{};

// ============================================================
// Solver
// ============================================================

Solver::Solver(const SolverConfig& config, TablebaseManager* tb_manager)
    : config_(config), tb_manager_(tb_manager) {}

// ---- 终局判定 ----

bool Solver::check_terminal(int k, uint32_t wolf_rank, uint32_t sheep_rank,
                            bool turn, uint8_t& result, uint8_t& distance) {
    // k < 4 → 狼胜（羊不足 4）
    if (k < 4) {
        result = TB_WOLF_WIN;
        distance = 0;
        return true;
    }

    // 解码局面
    const auto& wolf_info = WOLF_INFO[wolf_rank];
    auto sheep_pos = decode_sheep(wolf_info.free_list, sheep_rank, k);

    // 构建位板
    uint32_t sheep_bb = 0;
    for (int sp : sheep_pos) {
        sheep_bb |= (1u << sp);
    }
    uint32_t wolf_bb = 0;
    for (int wp : wolf_info.positions) {
        wolf_bb |= (1u << wp);
    }
    uint32_t occ = sheep_bb | wolf_bb;

    // 检查三狼是否都无法移动
    bool all_blocked = true;
    for (int wp : wolf_info.positions) {
        int r = row_of(wp);
        int c = col_of(wp);
        for (int d = 0; d < 4; ++d) {
            int adj = MOVE_TABLE.adjacent[wp][d];
            if (adj >= 0 && !(occ & (1u << adj))) {
                all_blocked = false;
                break;
            }
        }
        if (!all_blocked) break;
    }

    if (all_blocked) {
        result = TB_SHEEP_WIN;
        distance = 0;
        return true;
    }

    return false;
}

// ---- 后继生成 ----

void Solver::gen_successors(int k, uint32_t wolf_rank, uint32_t sheep_rank,
                            bool turn,
                            const std::function<void(uint64_t, int)>& callback) {
    const auto& wolf_info = WOLF_INFO[wolf_rank];
    auto sheep_pos = decode_sheep(wolf_info.free_list, sheep_rank, k);

    // 构建位板
    uint32_t sheep_bb = 0;
    for (int sp : sheep_pos) {
        sheep_bb |= (1u << sp);
    }
    uint32_t wolf_bb = 0;
    for (int wp : wolf_info.positions) {
        wolf_bb |= (1u << wp);
    }
    uint32_t occ = sheep_bb | wolf_bb;

    if (turn) {
        // ---- 羊回合 ----
        // 每只羊尝试移动
        uint32_t sheep_bits = sheep_bb;
        while (sheep_bits) {
            int from = std::countr_zero(sheep_bits);
            sheep_bits &= sheep_bits - 1;

            for (int d = 0; d < 4; ++d) {
                int to = MOVE_TABLE.adjacent[from][d];
                if (to < 0) continue;
                if (occ & (1u << to)) continue;  // 目标格被占

                // 构建新羊位板
                uint32_t new_sheep_bb = (sheep_bb & ~(1u << from)) | (1u << to);

                // 计算新 sheep_rank
                std::vector<int> new_sheep_pos;
                uint32_t bb = new_sheep_bb;
                while (bb) {
                    int idx = std::countr_zero(bb);
                    bb &= bb - 1;
                    new_sheep_pos.push_back(idx);
                }
                uint32_t new_sheep_rank = encode_sheep(
                    wolf_info.free_list, new_sheep_pos, k);

                // 后继：狼回合，同一 k 桶
                uint64_t succ_idx = state_index(wolf_rank, new_sheep_rank,
                                                 k, false);
                callback(succ_idx, k);
            }
        }
    } else {
        // ---- 狼回合 ----
        for (int wp : wolf_info.positions) {
            int from = wp;

            for (int d = 0; d < 4; ++d) {
                // 简单移动
                int to = MOVE_TABLE.adjacent[from][d];
                if (to >= 0 && !(occ & (1u << to))) {
                    // 新狼位板
                    uint32_t new_wolf_bb = (wolf_bb & ~(1u << from)) | (1u << to);
                    uint32_t new_wolf_rank = encode_wolf_bb(new_wolf_bb);

                    // 新羊位板不变，但 free_list 变了
                    const auto& new_wolf_info = WOLF_INFO[new_wolf_rank];
                    uint32_t new_sheep_rank = encode_sheep(
                        new_wolf_info.free_list, sheep_pos, k);

                    // 后继：羊回合，同一 k 桶
                    uint64_t succ_idx = state_index(new_wolf_rank,
                                                     new_sheep_rank, k, true);
                    callback(succ_idx, k);
                }

                // 吃子
                int prey = MOVE_TABLE.capture_prey[from][d];
                int jumped = MOVE_TABLE.capture_jumped[from][d];
                if (prey >= 0 && (sheep_bb & (1u << prey)) &&
                    !(occ & (1u << jumped))) {
                    // 新狼位板：狼移到猎物位置
                    uint32_t new_wolf_bb = (wolf_bb & ~(1u << from)) | (1u << prey);
                    uint32_t new_wolf_rank = encode_wolf_bb(new_wolf_bb);

                    // 新羊位板：移除被吃的羊
                    uint32_t new_sheep_bb = sheep_bb & ~(1u << prey);
                    std::vector<int> new_sheep_pos;
                    uint32_t bb = new_sheep_bb;
                    while (bb) {
                        int idx = std::countr_zero(bb);
                        bb &= bb - 1;
                        new_sheep_pos.push_back(idx);
                    }

                    const auto& new_wolf_info = WOLF_INFO[new_wolf_rank];
                    uint32_t new_sheep_rank = encode_sheep(
                        new_wolf_info.free_list, new_sheep_pos, k - 1);

                    // 后继：羊回合，k-1 桶
                    uint64_t succ_idx = state_index(new_wolf_rank,
                                                     new_sheep_rank, k - 1, true);
                    callback(succ_idx, k - 1);
                }
            }
        }
    }
}

// ---- 状态评估 ----

bool Solver::evaluate_state(Tablebase& tb, int k, uint64_t idx,
                            uint32_t wolf_rank, uint32_t sheep_rank,
                            bool turn) {
    uint8_t old_val = tb.get(idx);
    uint8_t old_result = tb_result(old_val);
    if (old_result != TB_UNKNOWN) return false; // 已求解

    bool found_win = false;       // 找到致胜走法
    uint8_t best_win_dist = 255;  // 致胜走法的最短距离
    bool all_opponent_win = true; // 所有后继都是对手胜
    uint8_t max_opp_dist = 0;     // 对手胜的最大距离
    bool has_any_move = false;

    if (turn) {
        // ---- 羊回合：羊想找 SHEEP_WIN ----
        gen_successors(k, wolf_rank, sheep_rank, true,
            [&](uint64_t succ_idx, int succ_k) {
                has_any_move = true;

                uint8_t succ_val;
                if (succ_k == k) {
                    succ_val = tb.get(succ_idx);
                } else if (succ_k < 4) {
                    // k < 4 是平凡狼胜桶
                    succ_val = tb_pack(TB_WOLF_WIN, 0);
                } else {
                    Tablebase* prev_tb = tb_manager_->get_bucket(succ_k);
                    succ_val = prev_tb ? prev_tb->get(succ_idx)
                                       : tb_pack(TB_UNKNOWN, 0);
                }

                uint8_t sr = tb_result(succ_val);
                uint8_t sd = tb_distance(succ_val);

                if (sr == TB_SHEEP_WIN) {
                    found_win = true;
                    best_win_dist = std::min(best_win_dist,
                        static_cast<uint8_t>(sd + 1));
                }
                if (sr != TB_WOLF_WIN) {
                    all_opponent_win = false;
                } else {
                    max_opp_dist = std::max(max_opp_dist, sd);
                }
            });

        if (found_win) {
            tb.set(idx, TB_SHEEP_WIN, best_win_dist);
            return true;
        }
        if (has_any_move && all_opponent_win) {
            tb.set(idx, TB_WOLF_WIN,
                static_cast<uint8_t>(std::min(255, max_opp_dist + 1)));
            return true;
        }
    } else {
        // ---- 狼回合：狼想找 WOLF_WIN ----
        gen_successors(k, wolf_rank, sheep_rank, false,
            [&](uint64_t succ_idx, int succ_k) {
                has_any_move = true;

                uint8_t succ_val;
                if (succ_k == k) {
                    succ_val = tb.get(succ_idx);
                } else if (succ_k < 4) {
                    // k < 4 是平凡狼胜桶
                    succ_val = tb_pack(TB_WOLF_WIN, 0);
                } else {
                    Tablebase* prev_tb = tb_manager_->get_bucket(succ_k);
                    succ_val = prev_tb ? prev_tb->get(succ_idx)
                                       : tb_pack(TB_UNKNOWN, 0);
                }

                uint8_t sr = tb_result(succ_val);
                uint8_t sd = tb_distance(succ_val);

                if (sr == TB_WOLF_WIN) {
                    found_win = true;
                    best_win_dist = std::min(best_win_dist,
                        static_cast<uint8_t>(sd + 1));
                }
                if (sr != TB_SHEEP_WIN) {
                    all_opponent_win = false;
                } else {
                    max_opp_dist = std::max(max_opp_dist, sd);
                }
            });

        if (found_win) {
            tb.set(idx, TB_WOLF_WIN, best_win_dist);
            return true;
        }
        if (has_any_move && all_opponent_win) {
            tb.set(idx, TB_SHEEP_WIN,
                static_cast<uint8_t>(std::min(255, max_opp_dist + 1)));
            return true;
        }
    }

    return false;
}

// ---- 处理一个 wolf_rank 块 ----

void Solver::process_block(Tablebase& tb, int k,
                           uint32_t wolf_start, uint32_t wolf_end,
                           std::atomic<bool>& changed,
                           std::atomic<uint64_t>& solved_count) {
    uint64_t sheep_combos = BINOM[22][k];

    for (uint32_t wr = wolf_start; wr < wolf_end; ++wr) {
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            // 狼回合
            uint64_t idx_w = state_index(wr, sr, k, false);
            if (evaluate_state(tb, k, idx_w, wr, sr, false)) {
                changed = true;
                solved_count++;
            }

            // 羊回合
            uint64_t idx_s = state_index(wr, sr, k, true);
            if (evaluate_state(tb, k, idx_s, wr, sr, true)) {
                changed = true;
                solved_count++;
            }
        }
    }
}

// ---- 标记终局 ----

uint64_t Solver::mark_terminals(Tablebase& tb, int k) {
    uint64_t count = 0;
    uint64_t sheep_combos = BINOM[22][k];

    for (uint32_t wr = 0; wr < 2300; ++wr) {
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            for (int t = 0; t < 2; ++t) {
                bool turn = (t == 1);
                uint8_t result, distance;
                if (check_terminal(k, wr, sr, turn, result, distance)) {
                    uint64_t idx = state_index(wr, sr, k, turn);
                    tb.set(idx, result, distance);
                    count++;
                }
            }
        }
    }
    return count;
}

// ---- 求解单个桶 ----

bool Solver::solve_bucket(int k) {
    if (k < 4) {
        // k=1,2,3 是平凡狼胜桶，无需表库
        if (config_.verbose) {
            std::cout << "Bucket k=" << k << " is trivial (wolf win), skipping.\n";
        }
        return true;
    }

    if (config_.verbose) {
        std::cout << "\n=== Solving bucket k=" << k << " ===\n";
    }

    // 确保 k-1 桶已加载
    if (k > 4) {
        Tablebase* prev = tb_manager_->get_bucket(k - 1);
        if (!prev || !prev->is_open()) {
            std::cerr << "Error: k-1 bucket not available for k=" << k << "\n";
            return false;
        }
        if (tb_manager_->is_completed(k - 1)) {
            if (config_.verbose) {
                std::cout << "  k-1 bucket is completed.\n";
            }
        }
    }

    // 获取或创建当前桶
    Tablebase* tb = tb_manager_->get_bucket(k);
    if (!tb) {
        std::cerr << "Error: failed to create bucket k=" << k << "\n";
        return false;
    }

    // 检查是否已完成
    if (tb_manager_->is_completed(k)) {
        if (config_.verbose) {
            std::cout << "  Bucket k=" << k << " already completed, skipping.\n";
        }
        return true;
    }

    uint64_t total = bucket_size(k);
    if (config_.verbose) {
        std::cout << "  Total entries: " << total
                  << " (" << total / 1024 / 1024 << " MB)\n";
    }

    // 初始化全部为 UNKNOWN
    if (config_.verbose) std::cout << "  Initializing...\n";
    tb->fill_unknown();

    // 标记终局
    if (config_.verbose) std::cout << "  Marking terminals...\n";
    uint64_t terminal_count = mark_terminals(*tb, k);
    if (config_.verbose) {
        std::cout << "  Terminals: " << terminal_count << "\n";
    }

    // 迭代求解
    int num_blocks = std::max(1, 2300 / config_.block_size);
    if (config_.verbose) {
        std::cout << "  Blocks: " << num_blocks
                  << " (block_size=" << config_.block_size << ")\n";
    }

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
        std::atomic<bool> changed{false};
        std::atomic<uint64_t> solved_count{0};

        // 并行处理所有块
        std::vector<std::thread> threads;
        for (int b = 0; b < num_blocks; ++b) {
            uint32_t w_start = b * (2300 / num_blocks);
            uint32_t w_end = (b == num_blocks - 1) ? 2300
                             : (b + 1) * (2300 / num_blocks);

            threads.emplace_back([this, tb, k, w_start, w_end,
                                  &changed, &solved_count]() {
                process_block(*tb, k, w_start, w_end, changed, solved_count);
            });

            // 限制并发线程数
            if (threads.size() >= static_cast<size_t>(
                    std::max(1, config_.num_threads))) {
                for (auto& t : threads) t.join();
                threads.clear();
            }
        }
        for (auto& t : threads) t.join();

        uint64_t sc = solved_count.load();
        if (config_.verbose) {
            uint64_t unknown = total - terminal_count;
            float pct = unknown > 0 ? (100.0f * sc / unknown) : 100.0f;
            std::cout << "  Iter " << iter << ": solved " << sc
                      << " new states ("
                      << std::fixed << std::setprecision(1) << pct
                      << "% of unknown)\n";
        }

        if (!changed) {
            if (config_.verbose) {
                std::cout << "  Converged after " << iter << " iterations.\n";
            }
            break;
        }

        if (iter == config_.max_iterations - 1) {
            std::cerr << "  Warning: max iterations reached!\n";
        }
    }

    // 剩余 UNKNOWN → DRAW
    if (config_.verbose) std::cout << "  Setting remaining as DRAW...\n";
    uint64_t draw_count = 0;
    uint64_t sheep_combos = BINOM[22][k];
    for (uint32_t wr = 0; wr < 2300; ++wr) {
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            for (int t = 0; t < 2; ++t) {
                uint64_t idx = state_index(wr, sr, k, t);
                if (tb_result(tb->get(idx)) == TB_UNKNOWN) {
                    tb->set(idx, TB_DRAW, 0);
                    draw_count++;
                }
            }
        }
    }
    if (config_.verbose) {
        std::cout << "  Draws: " << draw_count << "\n";
    }

    // 标记完成
    tb->mark_completed();
    if (config_.verbose) {
        std::cout << "  Bucket k=" << k << " completed and saved.\n";
    }

    return true;
}

// ---- 求解所有桶 ----

bool Solver::solve_all() {
    // k=4 到 15 依次求解
    for (int k = 4; k <= 15; ++k) {
        if (!solve_bucket(k)) {
            std::cerr << "Failed to solve bucket k=" << k << "\n";
            return false;
        }
        progress_ = static_cast<float>(k - 3) / 12.0f;
    }
    progress_ = 1.0f;
    return true;
}

} // namespace wolves