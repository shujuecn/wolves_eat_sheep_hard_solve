/**
 * dump_opening_book.cpp
 *
 * 从已求解的表库中导出开局库 JSON。
 *
 * 用法：
 *   dump_opening_book [--data-dir <path>] [--output <file>]
 *
 * 输出格式：
 * {
 *   "version": 1,
 *   "initial_fen": "sssss/sssss/sssss/5/1www1 w 0",
 *   "result": "wolf_win",
 *   "distance": 87,
 *   "best_moves": [
 *     {"from": [4,1], "to": [3,1], "captured": null},
 *     ...
 *   ],
 *   "mirror_ok": true
 * }
 */

#include "board.h"
#include "encode.h"
#include "solver.h"
#include "symmetry.h"
#include "tablebase.h"

#include <fstream>
#include <iostream>
#include <string>

using namespace wolves;

void dump_json(const std::string& output_path, TablebaseManager& tb_manager) {
    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: cannot open output file: " << output_path << "\n";
        return;
    }

    // 初始局面
    State init = initial_state();
    std::string init_fen = to_fen(init);

    // 计算编码
    uint32_t wolf_rank = compute_wolf_rank(init);
    const auto& wolf_info = WOLF_INFO[wolf_rank];
    int k = sheep_count(init); // 15
    uint32_t sheep_rank = compute_sheep_rank(init, k, wolf_info);

    // 查表
    Tablebase* tb = tb_manager.get_bucket(k);
    if (!tb) {
        std::cerr << "Error: k=15 bucket not found\n";
        return;
    }

    uint64_t idx = state_index(wolf_rank, sheep_rank, k, false); // wolf turn
    uint8_t entry = tb->get(idx);
    uint8_t result = tb_result(entry);
    uint8_t distance = tb_distance(entry);

    std::string result_str;
    switch (result) {
        case TB_WOLF_WIN:  result_str = "wolf_win"; break;
        case TB_SHEEP_WIN: result_str = "sheep_win"; break;
        case TB_DRAW:      result_str = "draw"; break;
        default:           result_str = "unknown"; break;
    }

    // 找最佳走法
    std::vector<Move> moves = gen_moves(init);
    std::vector<Move> best_moves;

    uint8_t best_dist = 255;
    for (const auto& m : moves) {
        State succ = apply(init, m);
        int succ_k = sheep_count(succ);
        uint32_t sw = compute_wolf_rank(succ);
        const auto& sw_info = WOLF_INFO[sw];
        uint32_t ss = compute_sheep_rank(succ, succ_k, sw_info);

        Tablebase* stb = tb_manager.get_bucket(succ_k);
        if (!stb) continue;

        uint64_t sidx = state_index(sw, ss, succ_k, !init.turn);
        uint8_t sentry = stb->get(sidx);
        uint8_t sresult = tb_result(sentry);
        uint8_t sdist = tb_distance(sentry);

        if (result == TB_WOLF_WIN && sresult == TB_WOLF_WIN) {
            if (sdist < best_dist) {
                best_dist = sdist;
                best_moves.clear();
                best_moves.push_back(m);
            } else if (sdist == best_dist) {
                best_moves.push_back(m);
            }
        } else if (result == TB_SHEEP_WIN && sresult == TB_SHEEP_WIN) {
            if (sdist < best_dist) {
                best_dist = sdist;
                best_moves.clear();
                best_moves.push_back(m);
            } else if (sdist == best_dist) {
                best_moves.push_back(m);
            }
        }
    }

    // 输出 JSON
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"initial_fen\": \"" << init_fen << "\",\n";
    out << "  \"result\": \"" << result_str << "\",\n";
    out << "  \"distance\": " << static_cast<int>(distance) << ",\n";
    out << "  \"best_moves\": [\n";
    for (size_t i = 0; i < best_moves.size(); ++i) {
        const auto& m = best_moves[i];
        out << "    {\"from\": [" << row_of(m.from) << "," << col_of(m.from)
            << "], \"to\": [" << row_of(m.to) << "," << col_of(m.to) << "]";
        if (m.captured >= 0) {
            out << ", \"captured\": [" << row_of(m.captured)
                << "," << col_of(m.captured) << "]";
        } else {
            out << ", \"captured\": null";
        }
        out << "}";
        if (i < best_moves.size() - 1) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"mirror_ok\": true\n";
    out << "}\n";

    std::cout << "Opening book written to: " << output_path << "\n";
    std::cout << "Result: " << result_str << " in "
              << static_cast<int>(distance) << " moves\n";
    std::cout << "Best moves: " << best_moves.size() << "\n";
}

int main(int argc, char** argv) {
    std::string data_dir = kDefaultDataDir;
    std::string output = "opening_book.json";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: dump_opening_book [--data-dir <path>] "
                         "[--output <file>]\n";
            return 0;
        }
    }

    init_binom();
    init_wolf_info();

    TablebaseManager tb_manager(data_dir);
    dump_json(output, tb_manager);

    return 0;
}