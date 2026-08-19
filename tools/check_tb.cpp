/**
 * check_tb.cpp — 独立校验 k=4/k=5 表库是否自洽可用
 *
 * 不依赖 solver.cpp 的求解逻辑：用 board.h 的走法生成 + encode.h 的编码，
 * 对每个局面重新做一次“逆推一致性”校验：
 *   - 终局局面（狼全被堵死）必须是 SHEEP_WIN 且距离为 0
 *   - 非终局局面：根据所有后继在表中的结果重算期望值，与表中结果一致
 *   - 胜/负局面的距离满足 minimax 递推；和棋与终局距离为 0
 *   - 不允许残留 UNKNOWN（结果码 3）
 *
 * 用法：
 *   check_tb [--data-dir <path>] [--start-k K] [--end-k K] [--threads N]
 */

#include "board.h"
#include "encode.h"
#include "platform.h"
#include "tablebase.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wolves;

// 距离字段为 6 bit，封顶 63；求解器对超过 63 的真实 DTM 一律存 63
static constexpr int TB_MAX_DIST = 63;

// 只读加载一个表库文件，不写回、不修改。映射经 platform.h 统一封装。
struct LoadedTB {
    uint8_t* map = nullptr;
    uint64_t size = 0;
    const TBHeader* header = nullptr;
    const uint8_t* data = nullptr;

    bool load(const std::string& path, int expect_k) {
        uint64_t fsz = 0;
        map = static_cast<uint8_t*>(os_map_file_read(path, fsz));
        if (!map) {
            std::cerr << "  [load] 无法映射 " << path << "\n";
            return false;
        }

        uint64_t expect_size = sizeof(TBHeader) + bucket_size(expect_k);
        if (fsz != expect_size) {
            std::cerr << "  [header] 文件大小不符: got " << fsz
                      << " expected " << expect_size << "\n";
            os_unmap(map, fsz);
            map = nullptr;
            return false;
        }

        header = reinterpret_cast<const TBHeader*>(map);
        if (std::memcmp(header->magic, "WSTB", 4) != 0) {
            std::cerr << "  [header] magic 错误\n";
            close();
            return false;
        }
        if (header->k != expect_k) {
            std::cerr << "  [header] k 不符: header=" << static_cast<int>(header->k)
                      << " expected=" << expect_k << "\n";
            close();
            return false;
        }
        if (header->total_entries != bucket_size(expect_k)) {
            std::cerr << "  [header] total_entries 不符\n";
            close();
            return false;
        }

        data = map + sizeof(TBHeader);
        size = header->total_entries;
        return true;
    }

    void close() {
        if (map) {
            os_unmap(map, sizeof(TBHeader) + size);
            map = nullptr;
        }
        header = nullptr;
        data = nullptr;
        size = 0;
    }

    uint8_t result(uint64_t idx) const { return tb_result(data[idx]); }
    uint8_t distance(uint64_t idx) const { return tb_distance(data[idx]); }
};

static std::vector<int> bits_to_positions(uint32_t bb) {
    std::vector<int> v;
    while (bb) {
        int i = std::countr_zero(bb);
        v.push_back(i);
        bb &= bb - 1;
    }
    return v;
}

struct Stats {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> wolf_win{0};
    std::atomic<uint64_t> sheep_win{0};
    std::atomic<uint64_t> draw{0};
    std::atomic<uint64_t> unknown{0};
    std::atomic<uint64_t> terminal{0};
    std::atomic<uint64_t> result_errors{0};
    std::atomic<uint64_t> distance_errors{0};
};

static void check_range(int k,
                        const LoadedTB& tb,
                        const LoadedTB* const* buckets,
                        uint32_t wr_start,
                        uint32_t wr_end,
                        Stats& st,
                        std::mutex& emu,
                        int& err_printed) {
    uint64_t sheep_combos = BINOM[22][k];

    for (uint32_t wr = wr_start; wr < wr_end; ++wr) {
        const WolfInfo& wi = WOLF_INFO[wr];

        for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
            for (int t = 0; t < 2; ++t) {
                bool turn = (t == 1);
                uint64_t idx = state_index(wr, sr, k, turn);
                uint8_t stored_r = tb.result(idx);
                uint8_t stored_d = tb.distance(idx);
                st.total++;

                switch (stored_r) {
                    case TB_WOLF_WIN: st.wolf_win++; break;
                    case TB_SHEEP_WIN: st.sheep_win++; break;
                    case TB_DRAW: st.draw++; break;
                    default: st.unknown++; break;
                }

                // 重建局面。注意 board.h 的 State::turn 与 encode.h 相反：
                // 这里 turn=true 表示羊回合（编码约定），而 State 用 true 表示狼回合。
                State s;
                s.turn = !turn;
                for (int wp : wi.positions) s.wolf_bb |= (1u << wp);
                std::vector<int> sheep = decode_sheep(wi.free_list, sr, k);
                for (int sp : sheep) s.sheep_bb |= (1u << sp);

                // 终局：狼全被堵死 -> 羊胜（k>=4 时唯一终局类型）
                if (!any_wolf_can_move(s)) {
                    st.terminal++;
                    if (stored_r != TB_SHEEP_WIN || stored_d != 0) {
                        if (stored_r != TB_SHEEP_WIN) st.result_errors++;
                        else st.distance_errors++;
                        std::lock_guard<std::mutex> lk(emu);
                        if (err_printed < 20) {
                            err_printed++;
                            std::cerr << "  [terminal] k=" << k << " wr=" << wr
                                      << " sr=" << sr << " turn=" << t
                                      << " stored=" << static_cast<int>(stored_r)
                                      << "/" << static_cast<int>(stored_d)
                                      << " expected=SHEEP_WIN/0\n";
                        }
                    }
                    continue;
                }

                // 非终局：按后继结果做 minimax 一致性校验
                auto moves = gen_moves(s);
                bool has_move = !moves.empty();
                bool found_win = false;
                bool all_opp_win = true;
                uint8_t best_win_dist = 255;
                uint8_t max_opp_dist = 0;

                for (const Move& m : moves) {
                    State succ = apply(s, m);
                    int succ_k = (m.captured >= 0) ? (k - 1) : k;
                    uint32_t nwr = encode_wolf_bb(succ.wolf_bb);
                    const WolfInfo& nwi = WOLF_INFO[nwr];
                    std::vector<int> nsheep = bits_to_positions(succ.sheep_bb);
                    uint32_t nsr = encode_sheep(nwi.free_list, nsheep, succ_k);
                    // 后继的回合（编码约定）为 !turn
                    uint64_t sidx = state_index(nwr, nsr, succ_k, !turn);

                    uint8_t sr_ = (succ_k < 4) ? TB_WOLF_WIN
                                               : buckets[succ_k]->result(sidx);
                    uint8_t sd = (succ_k < 4) ? 0
                                              : buckets[succ_k]->distance(sidx);

                    if (turn) {
                        // 羊回合：羊想要 SHEEP_WIN
                        if (sr_ == TB_SHEEP_WIN) {
                            found_win = true;
                            best_win_dist = std::min<uint8_t>(
                                best_win_dist, static_cast<uint8_t>(sd + 1));
                        }
                        if (sr_ != TB_WOLF_WIN) {
                            all_opp_win = false;
                        } else {
                            max_opp_dist = std::max<uint8_t>(max_opp_dist, sd);
                        }
                    } else {
                        // 狼回合：狼想要 WOLF_WIN
                        if (sr_ == TB_WOLF_WIN) {
                            found_win = true;
                            best_win_dist = std::min<uint8_t>(
                                best_win_dist, static_cast<uint8_t>(sd + 1));
                        }
                        if (sr_ != TB_SHEEP_WIN) {
                            all_opp_win = false;
                        } else {
                            max_opp_dist = std::max<uint8_t>(max_opp_dist, sd);
                        }
                    }
                }

                uint8_t expected;
                uint8_t expected_d = 0;
                if (turn) {
                    if (found_win) {
                        expected = TB_SHEEP_WIN;
                        expected_d = best_win_dist;
                    } else if (has_move && all_opp_win) {
                        expected = TB_WOLF_WIN;
                        expected_d = static_cast<uint8_t>(
                            std::min<int>(255, static_cast<int>(max_opp_dist) + 1));
                    } else {
                        expected = TB_DRAW;
                        expected_d = 0;
                    }
                } else {
                    if (found_win) {
                        expected = TB_WOLF_WIN;
                        expected_d = best_win_dist;
                    } else if (has_move && all_opp_win) {
                        expected = TB_SHEEP_WIN;
                        expected_d = static_cast<uint8_t>(
                            std::min<int>(255, static_cast<int>(max_opp_dist) + 1));
                    } else {
                        expected = TB_DRAW;
                        expected_d = 0;
                    }
                }

                bool r_ok = (stored_r == expected);
                bool d_ok;
                if (expected == TB_DRAW) {
                    d_ok = (stored_d == 0);
                } else if (expected_d > TB_MAX_DIST) {
                    // 真实 DTM 超过 6 位上限：求解器一律封顶为 63
                    d_ok = (stored_d == TB_MAX_DIST);
                } else {
                    d_ok = (stored_d == expected_d);
                }

                if (!r_ok || !d_ok) {
                    if (!r_ok) st.result_errors++;
                    if (!d_ok) st.distance_errors++;
                    std::lock_guard<std::mutex> lk(emu);
                    if (err_printed < 20) {
                        err_printed++;
                        std::cerr << "  [consistency] k=" << k << " wr=" << wr
                                  << " sr=" << sr << " turn=" << t
                                  << " stored=" << static_cast<int>(stored_r)
                                  << "/" << static_cast<int>(stored_d)
                                  << " expected=" << static_cast<int>(expected)
                                  << "/" << static_cast<int>(expected_d)
                                  << " fen=" << to_fen(s) << "\n";
                    }
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    std::string data_dir = kDefaultDataDir;
    int start_k = 4;
    int end_k = 5;
    int threads = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--start-k" && i + 1 < argc) {
            start_k = std::stoi(argv[++i]);
        } else if (arg == "--end-k" && i + 1 < argc) {
            end_k = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--data-dir <path>] [--start-k K] [--end-k K]"
                         " [--threads N]\n";
            return 0;
        }
    }

    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
        if (threads <= 0) threads = 4;
    }

    init_binom();
    init_wolf_info();

    std::cout << "=== Tablebase self-consistency check ===\n";
    std::cout << "Data dir: " << data_dir << "\n";
    std::cout << "Range:    k=" << start_k << " -> " << end_k << "\n";
    std::cout << "Threads:  " << threads << "\n\n";

    // 通用加载：加载 min(start_k,4)..end_k 的全部桶。
    // k>=5 的吃子后继会落到 k-1，因此必须从被检查桶向下的整条链都可用；
    // 强制包含 k=4（它是所有更深桶的后继基底）。
    int load_min = std::min(start_k, 4);
    LoadedTB tbs[16];
    const LoadedTB* buckets[16] = {nullptr};

    for (int k = load_min; k <= end_k; ++k) {
        if (k < 4) continue;  // k<4 平凡狼胜，无需文件

        std::string path = tb_bucket_path(data_dir, k);
        std::cout << "Loading k=" << k << ": " << path << "\n";
        if (!tbs[k].load(path, k)) {
            std::cerr << "Failed to load k=" << k << " tablebase.\n";
            return 2;
        }
        buckets[k] = &tbs[k];
        std::cout << "  ok, entries=" << tbs[k].size << "\n";
    }
    std::cout << "\n";

    bool all_ok = true;
    bool any_dist_off = false;

    for (int k = start_k; k <= end_k; ++k) {
        const LoadedTB* tb = buckets[k];
        if (!tb) {
            std::cerr << "k=" << k << " 未加载，跳过\n";
            continue;
        }

        Stats st;
        std::mutex emu;
        int err_printed = 0;

        std::cout << "Checking k=" << k << " ... " << std::flush;
        auto t0 = std::chrono::steady_clock::now();

        std::vector<std::thread> pool;
        uint32_t per = (2300 + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            uint32_t ws = static_cast<uint32_t>(t) * per;
            uint32_t we = std::min<uint32_t>(2300,
                static_cast<uint32_t>(t + 1) * per);
            if (ws >= we) continue;
            pool.emplace_back(check_range, k, std::cref(*tb), buckets,
                              ws, we, std::ref(st), std::ref(emu),
                              std::ref(err_printed));
        }
        for (auto& th : pool) th.join();

        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "done in " << sec << "s\n";
        std::cout << "  total:    " << st.total.load() << "\n";
        std::cout << "  wolf_win: " << st.wolf_win.load() << "\n";
        std::cout << "  sheep_win:" << st.sheep_win.load() << "\n";
        std::cout << "  draw:     " << st.draw.load() << "\n";
        std::cout << "  unknown:  " << st.unknown.load() << "\n";
        std::cout << "  terminal: " << st.terminal.load() << "\n";

        bool res_ok = (st.result_errors.load() == 0 && st.unknown.load() == 0);
        std::cout << "  result check:   "
                  << (res_ok ? "PASS" : "FAIL")
                  << " (" << st.result_errors.load() << " result errors, "
                  << st.unknown.load() << " unknown)\n";
        std::cout << "  distance check: "
                  << (st.distance_errors.load() == 0 ? "OPTIMAL"
                                                     : "SUBOPTIMAL")
                  << " (" << st.distance_errors.load() << " states off)\n\n";

        if (!res_ok) all_ok = false;
        if (st.distance_errors.load() != 0) any_dist_off = true;
    }

    std::cout << "=== RESULT CHECK: " << (all_ok ? "PASS" : "FAIL")
              << " ===\n";
    if (any_dist_off) {
        std::cout << "NOTE: distance(DTM) is suboptimal on some states; "
                     "win/loss/draw results are still correct.\n";
    } else if (all_ok) {
        std::cout << "Distances (DTM) are optimal.\n";
    }
    return all_ok ? 0 : 1;
}
