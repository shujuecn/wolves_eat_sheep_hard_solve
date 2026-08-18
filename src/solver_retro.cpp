#include "solver_retro.h"
#include "solver.h"  // MOVE_TABLE

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace wolves {

// ============================================================
// 原子辅助（mmap 内存上的 uint8 原子操作）
// ============================================================

static inline uint8_t atomic_load_u8(const uint8_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline bool atomic_cas_u8(uint8_t* p, uint8_t expected, uint8_t desired) {
    uint8_t e = expected;
    return __atomic_compare_exchange_n(p, &e, desired, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static inline uint8_t atomic_fetch_sub_u8(uint8_t* p, uint8_t v) {
    return __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
}

// 距离上限（表库 distance 字段为 6 bit，0..63）
static constexpr int MAX_DIST = 63;

// ============================================================
// RetroMoveTable 构建
// ============================================================

void RetroMoveTable::build() {
    wolf_moves.resize(2300);
    rev_wolf_simple.resize(2300);
    sheep_adj.resize(2300);
    wolf_adj_free.resize(2300);

    for (uint32_t wr = 0; wr < 2300; ++wr) {
        const auto& info = WOLF_INFO[wr];

        // ---- 羊邻接表（free_idx 空间） ----
        sheep_adj[wr].resize(22);
        for (int fi = 0; fi < 22; ++fi) {
            int gp = info.free_list[fi];
            for (int d = 0; d < 4; ++d) {
                int adj = MOVE_TABLE.adjacent[gp][d];
                if (adj < 0) continue;
                int adj_fi = info.global_to_free[adj];
                if (adj_fi >= 0) sheep_adj[wr][fi].push_back(adj_fi);
            }
        }

        // ---- 狼相邻非狼格（用于终局“三狼全堵死”判定） ----
        for (int wi = 0; wi < 3; ++wi) {
            int wp = info.positions[wi];
            for (int d = 0; d < 4; ++d) {
                int adj = MOVE_TABLE.adjacent[wp][d];
                if (adj < 0) continue;
                int adj_fi = info.global_to_free[adj];
                if (adj_fi >= 0) wolf_adj_free[wr][wi].push_back(adj_fi);
            }
        }

        // ---- 狼移动表 ----
        for (int wi = 0; wi < 3; ++wi) {
            int from = info.positions[wi];
            for (int d = 0; d < 4; ++d) {
                // 普通移动
                int to = MOVE_TABLE.adjacent[from][d];
                if (to >= 0 && info.global_to_free[to] >= 0) {
                    uint32_t new_bb = 0;
                    for (int wj = 0; wj < 3; ++wj) {
                        int wp = info.positions[wj];
                        if (wp == from) wp = to;
                        new_bb |= (1u << wp);
                    }
                    uint32_t new_wr = encode_wolf_bb(new_bb);

                    WolfMove m;
                    m.from_pos = from;
                    m.to_pos = to;
                    m.jumped_pos = -1;
                    m.new_wolf_rank = new_wr;
                    wolf_moves[wr].push_back(m);

                    // 反向普通移动：当前 new_wr 的狼位于 to，前驱狼位于 from
                    RevWolfSimple rev;
                    rev.prev_wolf_rank = wr;
                    rev.cur_pos = to;
                    rev.prev_pos = from;
                    rev_wolf_simple[new_wr].push_back(rev);
                }

                // 吃子移动
                int prey = MOVE_TABLE.capture_prey[from][d];
                int jumped = MOVE_TABLE.capture_jumped[from][d];
                if (prey >= 0 && info.global_to_free[prey] >= 0 &&
                    info.global_to_free[jumped] >= 0) {
                    uint32_t new_bb = 0;
                    for (int wj = 0; wj < 3; ++wj) {
                        int wp = info.positions[wj];
                        if (wp == from) wp = prey;
                        new_bb |= (1u << wp);
                    }
                    uint32_t new_wr = encode_wolf_bb(new_bb);

                    WolfMove m;
                    m.from_pos = from;
                    m.to_pos = prey;
                    m.jumped_pos = jumped;
                    m.new_wolf_rank = new_wr;
                    wolf_moves[wr].push_back(m);
                }
            }
        }
    }
}

RetroMoveTable RETRO_MOVE_TABLE;

// ============================================================
// 计数器管理
// ============================================================

bool RetrogradeSolver::alloc_counters(uint64_t size) {
    free_counters();
    counters_size_ = size;
    // 匿名 mmap：由操作系统按需零填充，无需 memset
    counters_ = static_cast<uint8_t*>(
        ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (counters_ == MAP_FAILED) {
        perror("mmap counters");
        counters_ = nullptr;
        counters_size_ = 0;
        return false;
    }
    loss_dists_size_ = size;
    loss_dists_ = static_cast<uint8_t*>(
        ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (loss_dists_ == MAP_FAILED) {
        perror("mmap loss_dists");
        ::munmap(counters_, counters_size_);
        counters_ = nullptr;
        counters_size_ = 0;
        loss_dists_ = nullptr;
        loss_dists_size_ = 0;
        return false;
    }
    return true;
}

void RetrogradeSolver::free_counters() {
    if (loss_dists_) {
        ::munmap(loss_dists_, loss_dists_size_);
        loss_dists_ = nullptr;
        loss_dists_size_ = 0;
    }
    if (counters_) {
        ::munmap(counters_, counters_size_);
        counters_ = nullptr;
        counters_size_ = 0;
    }
}

// ============================================================
// 求解器
// ============================================================

RetrogradeSolver::RetrogradeSolver(const RetroSolverConfig& config,
                                   TablebaseManager* tb_manager)
    : config_(config), tb_manager_(tb_manager) {}

namespace {

// 解码 sheep_rank -> free_idx（升序，0..21）
inline void decode_sheep_fi(uint32_t sheep_rank, int k, int* out) {
    decode_combination(sheep_rank, 22, k, out);
}

// 构建 free_idx -> sheep 下标 映射
inline void build_fi_to_si(const int* sheep_fi, int k, int* fi_to_si) {
    for (int i = 0; i < 22; ++i) fi_to_si[i] = -1;
    for (int i = 0; i < k; ++i) fi_to_si[sheep_fi[i]] = i;
}

// 编码一个（可能未排序的）free_idx 数组：先复制排序再编码
inline uint32_t encode_fi_sorted_copy(int* fi, int k) {
    std::sort(fi, fi + k);
    return encode_combination(fi, k);
}

} // namespace

// ---- Phase 1：狼回合 init ----
// 对每个 wolf-turn 状态：
//   - 无任何后继 -> SHEEP_WIN 终局（dist 0）
//   - 有吃子到 k-1 的 WOLF_WIN -> 立即 WOLF_WIN
//   - 所有后继均为 LOSS -> SHEEP_WIN
//   - 否则记录 cnt（未决状态）
void RetrogradeSolver::init_wolf_range(
    Tablebase& tb, int k, uint32_t wr_start, uint32_t wr_end,
    std::vector<std::vector<uint32_t>>& local_buckets) {

    uint64_t sheep_combos = BINOM[22][k];
    Tablebase* prev_tb = (k > 4) ? tb_manager_->get_bucket(k - 1) : nullptr;

    for (uint32_t wr = wr_start; wr < wr_end; ++wr) {
        const auto& info = WOLF_INFO[wr];

        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            int sheep_fi[22];
            decode_sheep_fi(sr, k, sheep_fi);
            int fi_to_si[22];
            build_fi_to_si(sheep_fi, k, fi_to_si);

            uint64_t idx = state_index(wr, sr, k, false);  // wolf turn

            bool found_win = false;
            uint8_t best_win = 0xFF;
            uint8_t cnt = 0;
            uint8_t max_loss = 0;
            bool has_any = false;

            for (const WolfMove& m : RETRO_MOVE_TABLE.wolf_moves[wr]) {
                if (m.jumped_pos < 0) {
                    // 普通移动：目标必须空闲（无羊）
                    int to_fi = info.global_to_free[m.to_pos];
                    if (to_fi < 0 || fi_to_si[to_fi] >= 0) continue;
                    // 同桶羊回合后继，当前为 UNKNOWN -> 计入 cnt
                    has_any = true;
                    cnt++;
                } else {
                    // 吃子：猎物有羊且中间格无羊
                    int prey_fi = info.global_to_free[m.to_pos];
                    int jumped_fi = info.global_to_free[m.jumped_pos];
                    if (prey_fi < 0 || jumped_fi < 0) continue;
                    int prey_si = fi_to_si[prey_fi];
                    if (prey_si < 0) continue;
                    if (fi_to_si[jumped_fi] >= 0) continue;

                    // 计算 k-1 后继索引
                    const auto& ninfo = WOLF_INFO[m.new_wolf_rank];
                    int nfi[22];
                    int nm = 0;
                    bool ok = true;
                    for (int i = 0; i < k; ++i) {
                        if (i == prey_si) continue;
                        int gp = info.free_list[sheep_fi[i]];
                        int nf = ninfo.global_to_free[gp];
                        if (nf < 0) { ok = false; break; }
                        nfi[nm++] = nf;
                    }
                    if (!ok) continue;
                    uint32_t new_sr = encode_fi_sorted_copy(nfi, k - 1);
                    uint64_t succ_idx = state_index(m.new_wolf_rank, new_sr,
                                                    k - 1, true);

                    uint8_t val;
                    if (k - 1 < 4) {
                        val = tb_pack(TB_WOLF_WIN, 0);
                    } else {
                        val = prev_tb->get(succ_idx);
                    }
                    uint8_t r = tb_result(val);
                    uint8_t dd = tb_distance(val);

                    has_any = true;
                    if (r == TB_WOLF_WIN) {
                        found_win = true;
                        best_win = std::min<uint8_t>(best_win,
                            static_cast<uint8_t>(std::min(MAX_DIST, dd + 1)));
                    } else if (r == TB_SHEEP_WIN) {
                        max_loss = std::max<uint8_t>(max_loss, dd);
                    } else {
                        cnt++;  // DRAW 或 UNKNOWN -> 非 LOSS
                    }
                }
            }

            if (!has_any) {
                // 三狼全被堵死：羊胜终局
                tb.set(idx, TB_SHEEP_WIN, 0);
                local_buckets[0].push_back(static_cast<uint32_t>(idx));
            } else if (found_win) {
                tb.set(idx, TB_WOLF_WIN, best_win);
                local_buckets[best_win].push_back(static_cast<uint32_t>(idx));
            } else if (cnt == 0) {
                uint8_t dist = static_cast<uint8_t>(
                    std::min(MAX_DIST, max_loss + 1));
                tb.set(idx, TB_SHEEP_WIN, dist);
                local_buckets[dist].push_back(static_cast<uint32_t>(idx));
            } else {
                counters_[idx] = cnt;
                // 记录跨桶吃子 LOSS 的最大距离，供传播阶段计算 LOSS 距离
                loss_dists_[idx] = max_loss;
            }
        }
    }
}

// ---- Phase 2：羊回合 init ----
void RetrogradeSolver::init_sheep_range(
    Tablebase& tb, int k, uint32_t wr_start, uint32_t wr_end,
    std::vector<std::vector<uint32_t>>& local_buckets) {

    uint64_t sheep_combos = BINOM[22][k];

    for (uint32_t wr = wr_start; wr < wr_end; ++wr) {
        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            int sheep_fi[22];
            decode_sheep_fi(sr, k, sheep_fi);
            int fi_to_si[22];
            build_fi_to_si(sheep_fi, k, fi_to_si);

            uint64_t idx = state_index(wr, sr, k, true);  // sheep turn

            // 终局：三狼全被堵死 -> 羊胜（与回合无关的局面属性）
            bool all_blocked = true;
            for (int wi = 0; wi < 3 && all_blocked; ++wi) {
                for (int adj_fi : RETRO_MOVE_TABLE.wolf_adj_free[wr][wi]) {
                    if (fi_to_si[adj_fi] < 0) { all_blocked = false; break; }
                }
            }
            if (all_blocked) {
                tb.set(idx, TB_SHEEP_WIN, 0);
                local_buckets[0].push_back(static_cast<uint32_t>(idx));
                continue;
            }

            bool found_win = false;
            uint8_t best_win = 0xFF;
            uint8_t cnt = 0;

            for (int si = 0; si < k; ++si) {
                int from_fi = sheep_fi[si];
                for (int to_fi : RETRO_MOVE_TABLE.sheep_adj[wr][from_fi]) {
                    if (fi_to_si[to_fi] >= 0) continue;  // 目标被羊占

                    // 后继 wolf-turn 状态（同桶）
                    int nfi[22];
                    for (int i = 0; i < k; ++i) nfi[i] = sheep_fi[i];
                    nfi[si] = to_fi;
                    uint32_t new_sr = encode_fi_sorted_copy(nfi, k);
                    uint64_t succ_idx = state_index(wr, new_sr, k, false);

                    uint8_t val = tb.get(succ_idx);
                    uint8_t r = tb_result(val);

                    if (r == TB_SHEEP_WIN) {
                        found_win = true;
                        best_win = std::min<uint8_t>(best_win,
                            static_cast<uint8_t>(std::min(MAX_DIST,
                                tb_distance(val) + 1)));
                    } else {
                        // WOLF_WIN 或 UNKNOWN 一律计入 cnt：
                        // WOLF_WIN 后继在传播阶段被处理时会对本状态 cnt 各
                        // 递减一次；UNKNOWN 后继在解析为 LOSS 时同样递减。
                        // 不能用 init 时的距离立即判负——该距离随后可能被
                        // 传播阶段“胜线升级”缩短，必须等全部后继以最终距离
                        // 处理完毕（cnt 归零）再判负，距离才最优。
                        cnt++;
                    }
                }
            }

            if (found_win) {
                tb.set(idx, TB_SHEEP_WIN, best_win);
                local_buckets[best_win].push_back(static_cast<uint32_t>(idx));
            } else {
                counters_[idx] = cnt;  // 包含已胜后继 + 待解析后继
            }
        }
    }
}

// ---- 传播阶段 ----
// cur 中的状态距离均为 wave_dist；新增状态（前驱）距离为 next_dist 或
// 更大（LOSS 路径并入跨桶捕获 max_loss）。out 携带 (索引, 实际距离)，
// 调用方按实际距离入桶。
void RetrogradeSolver::propagate_range(
    Tablebase& tb, int k, const std::vector<uint32_t>& cur,
    uint32_t lo, uint32_t hi, int wave_dist, int next_dist,
    std::vector<std::pair<uint32_t, uint8_t>>& out) {

    uint8_t nd = static_cast<uint8_t>(next_dist);
    uint8_t wd = static_cast<uint8_t>(wave_dist);
    uint8_t unknown_entry = tb_pack(TB_UNKNOWN, 0);

    for (uint32_t i = lo; i < hi; ++i) {
        uint64_t idx = cur[i];
        uint32_t wr = wolf_rank_from_index(idx, k);
        uint32_t sr = sheep_rank_from_index(idx, k);
        bool turn = turn_from_index(idx);

        uint8_t tval = atomic_load_u8(&tb.data()[idx]);
        uint8_t tr = tb_result(tval);

        // 距离守卫：状态可能因“距离降级”而被重复入桶（如 init 吃子胜被
        // 传播阶段更短的胜线覆盖）。若表中当前距离小于本波距离，说明该
        // 状态已按更优距离处理过，跳过陈旧条目，避免前驱计数被重复递减。
        if (tb_distance(tval) != wd) continue;

        // 前驱玩家视角：t 为羊回合 -> 前驱为狼回合；t 为狼回合 -> 前驱为羊回合
        uint8_t my_win = turn ? TB_WOLF_WIN : TB_SHEEP_WIN;
        uint8_t opp_win = turn ? TB_SHEEP_WIN : TB_WOLF_WIN;
        bool t_is_win_for_pred = (tr == my_win);

        int sheep_fi[22];
        decode_sheep_fi(sr, k, sheep_fi);
        const auto& info = WOLF_INFO[wr];

        if (turn) {
            // 羊回合 -> 狼回合前驱（反向狼普通移动）
            int fi_to_si[22];
            build_fi_to_si(sheep_fi, k, fi_to_si);

            for (const RevWolfSimple& rev : RETRO_MOVE_TABLE.rev_wolf_simple[wr]) {
                const auto& pinfo = WOLF_INFO[rev.prev_wolf_rank];
                int nfi[22];
                int nm = 0;
                bool ok = true;
                for (int j = 0; j < k; ++j) {
                    int gp = info.free_list[sheep_fi[j]];
                    int pf = pinfo.global_to_free[gp];
                    if (pf < 0) { ok = false; break; }
                    nfi[nm++] = pf;
                }
                if (!ok) continue;
                uint32_t prev_sr = encode_fi_sorted_copy(nfi, k);
                uint64_t prev_idx = state_index(rev.prev_wolf_rank, prev_sr,
                                                k, false);

                if (t_is_win_for_pred) {
                    bool got = false;
                    while (true) {
                        uint8_t cur_val = atomic_load_u8(&tb.data()[prev_idx]);
                        uint8_t cur_r = tb_result(cur_val);
                        // 已按更优（≤）距离赋相同结果 -> 跳过
                        if (cur_r == my_win && tb_distance(cur_val) <= nd) break;
                        // 已赋相反结果（不应发生，防御）-> 跳过
                        if (cur_r != TB_UNKNOWN && cur_r != my_win) break;
                        uint8_t desired = tb_pack(my_win, nd);
                        if (atomic_cas_u8(&tb.data()[prev_idx], cur_val,
                                          desired)) {
                            got = true;
                            break;
                        }
                    }
                    if (got) out.emplace_back(prev_idx, nd);
                } else if (tr == opp_win) {
                    uint8_t cur_val = atomic_load_u8(&tb.data()[prev_idx]);
                    if (tb_result(cur_val) != TB_UNKNOWN) continue;
                    uint8_t prev_cnt = atomic_fetch_sub_u8(
                        &counters_[prev_idx], 1);
                    if (prev_cnt == 1) {
                        uint8_t dist = static_cast<uint8_t>(std::min(
                            MAX_DIST, std::max<int>(nd,
                                static_cast<int>(loss_dists_[prev_idx]) + 1)));
                        uint8_t desired = tb_pack(opp_win, dist);
                        if (atomic_cas_u8(&tb.data()[prev_idx], unknown_entry,
                                          desired)) {
                            out.emplace_back(prev_idx, dist);
                        }
                    }
                }
            }
        } else {
            // 狼回合 -> 羊回合前驱（反向羊移动）
            int fi_to_si[22];
            build_fi_to_si(sheep_fi, k, fi_to_si);

            for (int si = 0; si < k; ++si) {
                int to_fi = sheep_fi[si];
                for (int from_fi : RETRO_MOVE_TABLE.sheep_adj[wr][to_fi]) {
                    if (fi_to_si[from_fi] >= 0) continue;

                    int nfi[22];
                    for (int j = 0; j < k; ++j) nfi[j] = sheep_fi[j];
                    nfi[si] = from_fi;
                    uint32_t prev_sr = encode_fi_sorted_copy(nfi, k);
                    uint64_t prev_idx = state_index(wr, prev_sr, k, true);

                    if (t_is_win_for_pred) {
                        bool got = false;
                        while (true) {
                            uint8_t cur_val = atomic_load_u8(
                                &tb.data()[prev_idx]);
                            uint8_t cur_r = tb_result(cur_val);
                            if (cur_r == my_win &&
                                tb_distance(cur_val) <= nd) break;
                            if (cur_r != TB_UNKNOWN && cur_r != my_win) break;
                            uint8_t desired = tb_pack(my_win, nd);
                            if (atomic_cas_u8(&tb.data()[prev_idx], cur_val,
                                              desired)) {
                                got = true;
                                break;
                            }
                        }
                        if (got) out.emplace_back(prev_idx, nd);
                    } else if (tr == opp_win) {
                        uint8_t cur_val = atomic_load_u8(&tb.data()[prev_idx]);
                        if (tb_result(cur_val) != TB_UNKNOWN) continue;
                        uint8_t prev_cnt = atomic_fetch_sub_u8(
                            &counters_[prev_idx], 1);
                        if (prev_cnt == 1) {
                            uint8_t dist = static_cast<uint8_t>(std::min(
                                MAX_DIST, std::max<int>(nd,
                                    static_cast<int>(loss_dists_[prev_idx]) + 1)));
                            uint8_t desired = tb_pack(opp_win, dist);
                            if (atomic_cas_u8(&tb.data()[prev_idx],
                                              unknown_entry, desired)) {
                                out.emplace_back(prev_idx, dist);
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---- 求解单个桶 ----

bool RetrogradeSolver::solve_bucket(int k) {
    if (k < 4) {
        if (config_.verbose) {
            std::cout << "Bucket k=" << k
                      << " is trivial (wolf win), skipping.\n";
        }
        return true;
    }

    if (config_.verbose) {
        std::cout << "\n=== [Retro] Solving bucket k=" << k << " ===\n";
    }

    if (k > 4) {
        Tablebase* prev = tb_manager_->get_bucket(k - 1);
        if (!prev || !prev->is_open()) {
            std::cerr << "Error: k-1 bucket not available for k=" << k << "\n";
            return false;
        }
    }

    Tablebase* tb = tb_manager_->get_bucket(k);
    if (!tb) {
        std::cerr << "Error: failed to create bucket k=" << k << "\n";
        return false;
    }

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

    if (config_.verbose) std::cout << "  Initializing tablebase...\n";
    tb->fill_unknown();

    if (config_.verbose) std::cout << "  Allocating counters...\n";
    if (!alloc_counters(total)) {
        std::cerr << "Error: failed to allocate counters\n";
        return false;
    }

    int num_threads = config_.num_threads;
    if (num_threads == 0) {
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
        if (num_threads == 0) num_threads = 4;
    }

    buckets_.assign(64, std::vector<uint32_t>());

    auto now_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    int64_t t_init1 = 0, t_init2 = 0, t_prop = 0, t_draw = 0;

    // 每个线程独立的 64 个距离桶，最后合并
    std::vector<std::vector<std::vector<uint32_t>>> locals(
        num_threads, std::vector<std::vector<uint32_t>>(64));

    auto run_wolf_blocks = [&](auto&& fn) {
        std::vector<std::thread> threads;
        uint32_t per = (2300 + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; ++t) {
            uint32_t ws = static_cast<uint32_t>(t) * per;
            uint32_t we = std::min<uint32_t>(2300, ws + per);
            if (ws >= we) continue;
            threads.emplace_back(fn, t, ws, we);
        }
        for (auto& th : threads) th.join();
    };

    // Phase 1：狼回合
    if (config_.verbose) std::cout << "  Init phase 1 (wolf turns)...\n";
    t_init1 = now_ms();
    run_wolf_blocks([&](int t, uint32_t ws, uint32_t we) {
        init_wolf_range(*tb, k, ws, we, locals[t]);
    });
    t_init1 = now_ms() - t_init1;

    // Phase 2：羊回合
    if (config_.verbose) std::cout << "  Init phase 2 (sheep turns)...\n";
    t_init2 = now_ms();
    run_wolf_blocks([&](int t, uint32_t ws, uint32_t we) {
        init_sheep_range(*tb, k, ws, we, locals[t]);
    });
    t_init2 = now_ms() - t_init2;

    // 合并局部桶
    uint64_t total_seeded = 0;
    for (int t = 0; t < num_threads; ++t) {
        for (int d = 0; d < 64; ++d) {
            if (locals[t][d].empty()) continue;
            total_seeded += locals[t][d].size();
            buckets_[d].insert(buckets_[d].end(),
                               locals[t][d].begin(), locals[t][d].end());
        }
    }
    // 释放局部桶内存
    locals.clear();
    locals.shrink_to_fit();

    if (config_.verbose) {
        std::cout << "  Seeded (win/loss/terminal): " << total_seeded << "\n";
    }

    // 传播：按距离 d 从 0 到 63
    uint64_t total_solved = total_seeded;
    t_prop = now_ms();
    for (int d = 0; d <= MAX_DIST; ++d) {
        int nd = (d < MAX_DIST) ? (d + 1) : MAX_DIST;

        while (!buckets_[d].empty()) {
            std::vector<uint32_t> cur = std::move(buckets_[d]);
            buckets_[d].clear();

            std::vector<std::vector<std::pair<uint32_t, uint8_t>>> next(num_threads);
            size_t sz = cur.size();
            size_t chunk = std::max<size_t>(1, config_.chunk_size);
            size_t nchunks = (sz + chunk - 1) / chunk;

            std::vector<std::thread> threads;
            std::atomic<size_t> cursor{0};

            auto worker = [&](int tid) {
                std::vector<std::pair<uint32_t, uint8_t>> local_out;
                while (true) {
                    size_t c = cursor.fetch_add(1);
                    if (c >= nchunks) break;
                    uint32_t lo = static_cast<uint32_t>(c * chunk);
                    uint32_t hi = static_cast<uint32_t>(
                        std::min<size_t>(sz, (c + 1) * chunk));
                    propagate_range(*tb, k, cur, lo, hi, d, nd, local_out);
                }
                if (!local_out.empty()) {
                    next[tid] = std::move(local_out);
                }
            };

            int nthreads = std::min<int>(num_threads,
                static_cast<int>(std::max<size_t>(1, nchunks)));
            for (int t = 0; t < nthreads; ++t) {
                threads.emplace_back(worker, t);
            }
            for (auto& th : threads) th.join();

            uint64_t produced = 0;
            for (int t = 0; t < nthreads; ++t) {
                produced += next[t].size();
            }
            total_solved += produced;

            if (config_.verbose) {
                std::cout << "  d=" << d << " queue=" << sz
                          << " -> new=" << produced
                          << " (solved=" << total_solved << ")\n";
            }

            // 合并 next 到 buckets_[距离字段]：LOSS 路径的实际距离可能大于
            // nd（并入跨桶捕获 max_loss），必须按实际距离入桶，否则距离守卫
            // 会把它当作陈旧条目永久跳过。
            auto merge = [&](std::vector<std::pair<uint32_t, uint8_t>>& v) {
                for (const auto& p : v) {
                    buckets_[std::min<uint8_t>(p.second, MAX_DIST)].
                        push_back(p.first);
                }
            };
            if (nd == d) {
                // d == MAX_DIST：饱和距离，若有产出则继续处理
                for (int t = 0; t < nthreads; ++t) {
                    if (next[t].empty()) continue;
                    merge(next[t]);
                }
                if (produced == 0) break;
            } else {
                for (int t = 0; t < nthreads; ++t) {
                    if (next[t].empty()) continue;
                    merge(next[t]);
                }
                break;  // d < MAX_DIST：单次处理即可
            }
        }
    }

    // 释放计数器
    free_counters();

    t_prop = now_ms() - t_prop;

    // 剩余 UNKNOWN -> DRAW
    if (config_.verbose) std::cout << "  Setting remaining as DRAW...\n";
    t_draw = now_ms();
    uint64_t draw_count = 0;
    {
        std::vector<std::thread> threads;
        std::atomic<uint64_t> dc{0};
        uint32_t per = (2300 + num_threads - 1) / num_threads;
        uint64_t sheep_combos = BINOM[22][k];

        auto scan = [&](uint32_t ws, uint32_t we) {
            uint64_t local_draw = 0;
            for (uint32_t wr = ws; wr < we; ++wr) {
                for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
                    for (int t = 0; t < 2; ++t) {
                        uint64_t idx = state_index(wr, sr, k, t);
                        if (tb_result(tb->get(idx)) == TB_UNKNOWN) {
                            tb->set(idx, TB_DRAW, 0);
                            local_draw++;
                        }
                    }
                }
            }
            dc += local_draw;
        };

        for (int t = 0; t < num_threads; ++t) {
            uint32_t ws = static_cast<uint32_t>(t) * per;
            uint32_t we = std::min<uint32_t>(2300, ws + per);
            if (ws >= we) continue;
            threads.emplace_back(scan, ws, we);
        }
        for (auto& th : threads) th.join();
        draw_count = dc.load();
    }
    t_draw = now_ms() - t_draw;

    if (config_.verbose) {
        std::cout << "  Draws: " << draw_count << "\n";
        std::cout << "  Solved (non-draw): " << total_solved << " / " << total
                  << "\n";
        std::cout << "  [timing] init1=" << t_init1 << "ms init2=" << t_init2
                  << "ms propagate=" << t_prop << "ms draw=" << t_draw << "ms\n";
    }

    tb->mark_completed();
    if (config_.verbose) {
        std::cout << "  Bucket k=" << k << " completed and saved.\n";
    }
    return true;
}

// ---- 求解所有桶 ----

bool RetrogradeSolver::solve_all() {
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
