/**
 * verify.cpp
 *
 * 随机局面验证工具：对比对称性、终局判定、与 Python 对拍
 *
 * 用法：
 *   verify [--data-dir <path>] [--samples N] [--check-symmetry]
 */

#include "board.h"
#include "encode.h"
#include "solver.h"
#include "symmetry.h"
#include "tablebase.h"

#include <chrono>
#include <iostream>
#include <random>
#include <string>

using namespace wolves;

// 随机生成局面
State random_state(std::mt19937& rng, int k) {
    State s;
    s.turn = (rng() & 1);

    // 随机放置 k 只羊和 3 只狼在 25 格中（不相交）
    std::vector<int> cells(25);
    for (int i = 0; i < 25; ++i) cells[i] = i;
    std::shuffle(cells.begin(), cells.end(), rng);

    for (int i = 0; i < 3; ++i) {
        s.wolf_bb |= (1u << cells[i]);
    }
    for (int i = 0; i < k; ++i) {
        s.sheep_bb |= (1u << cells[3 + i]);
    }

    s.move_count = 0;
    return s;
}

bool check_symmetry(TablebaseManager& tb_manager, int samples) {
    std::mt19937 rng(42);
    int errors = 0;

    std::cout << "Checking mirror symmetry with " << samples
              << " random samples...\n";

    for (int i = 0; i < samples; ++i) {
        int k = 4 + (rng() % 12); // 4..15
        State s = random_state(rng, k);

        uint32_t wr, sr;
        canonical_ranks(s, k, wr, sr);

        Tablebase* tb = tb_manager.get_bucket(k);
        if (!tb) continue;

        uint64_t idx = state_index(wr, sr, k, s.turn);
        uint8_t result = tb->get_result(idx);

        // 检查镜像
        State ms = mirror_state(s);
        uint32_t mwr, msr;
        canonical_ranks(ms, k, mwr, msr);
        uint64_t midx = state_index(mwr, msr, k, ms.turn);
        uint8_t mresult = tb->get_result(midx);

        if (result != mresult) {
            std::cerr << "Symmetry error!\n";
            std::cerr << "  State: " << to_fen(s) << "\n";
            std::cerr << "  Result: " << static_cast<int>(result)
                      << " Mirror: " << static_cast<int>(mresult) << "\n";
            errors++;
            if (errors >= 10) {
                std::cerr << "Too many errors, stopping.\n";
                break;
            }
        }
    }

    if (errors == 0) {
        std::cout << "All " << samples << " samples passed symmetry check.\n";
        return true;
    } else {
        std::cout << errors << " symmetry errors found.\n";
        return false;
    }
}

bool check_terminal_consistency(TablebaseManager& tb_manager, int samples) {
    std::mt19937 rng(123);
    int errors = 0;

    std::cout << "Checking terminal consistency with " << samples
              << " samples...\n";

    for (int i = 0; i < samples; ++i) {
        int k = 4 + (rng() % 12);
        State s = random_state(rng, k);

        uint32_t wr, sr;
        canonical_ranks(s, k, wr, sr);

        // 直接判定终局
        Result term = is_terminal(s);

        // 查表库
        Tablebase* tb = tb_manager.get_bucket(k);
        if (!tb) continue;

        uint64_t idx = state_index(wr, sr, k, s.turn);
        uint8_t tb_result_val = tb->get_result(idx);

        // 如果直接判定是终局，表库应该一致
        if (term == Result::WOLF_WIN && tb_result_val != TB_WOLF_WIN) {
            std::cerr << "Terminal mismatch: direct=WOLF_WIN, tb="
                      << static_cast<int>(tb_result_val) << "\n";
            std::cerr << "  FEN: " << to_fen(s) << "\n";
            errors++;
        } else if (term == Result::SHEEP_WIN && tb_result_val != TB_SHEEP_WIN) {
            std::cerr << "Terminal mismatch: direct=SHEEP_WIN, tb="
                      << static_cast<int>(tb_result_val) << "\n";
            std::cerr << "  FEN: " << to_fen(s) << "\n";
            errors++;
        }

        if (errors >= 10) break;
    }

    if (errors == 0) {
        std::cout << "All terminal checks passed.\n";
        return true;
    } else {
        std::cout << errors << " terminal mismatches found.\n";
        return false;
    }
}

int main(int argc, char** argv) {
    std::string data_dir = kDefaultDataDir;
    int samples = 10000;
    bool do_symmetry = true;
    bool do_terminal = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--samples" && i + 1 < argc) {
            samples = std::stoi(argv[++i]);
        } else if (arg == "--check-symmetry") {
            do_symmetry = true;
            do_terminal = false;
        } else if (arg == "--check-terminal") {
            do_symmetry = false;
            do_terminal = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: verify [--data-dir <path>] [--samples N]\n";
            return 0;
        }
    }

    init_binom();
    init_wolf_info();

    TablebaseManager tb_manager(data_dir);

    bool ok = true;

    if (do_symmetry) {
        if (!check_symmetry(tb_manager, samples)) ok = false;
    }

    if (do_terminal) {
        if (!check_terminal_consistency(tb_manager, samples)) ok = false;
    }

    return ok ? 0 : 1;
}