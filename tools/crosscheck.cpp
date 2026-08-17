/**
 * crosscheck.cpp
 *
 * 从 stdin 读取 FEN 格式局面，输出所有合法走法。
 * 与 Python rules.py 的 legal_moves_from 逐条对拍。
 *
 * 输入格式（每行一个局面）：
 *   <fen>  或  # 注释行
 *
 * 输出格式（JSON 数组，每局面一个对象）：
 *   {"fen":"...", "moves":[{"from":[r,c],"to":[r,c],"captured":[r,c]|null}, ...]}
 */

#include "board.h"

#include <iostream>
#include <string>
#include <sstream>
#include <cctype>

using namespace wolves;

// 解析 FEN 字符串为 State
// 格式：<rows>/<rows>/<rows>/<rows>/<rows> <turn> <move_count>
// 例：5s5s5s/5/3w3w3w w 0
bool parse_fen(const std::string& fen, State& s) {
    s = State{};
    int r = 0, c = 0;

    size_t i = 0;
    // 解析棋盘
    while (i < fen.size() && r < BOARD_SIZE) {
        char ch = fen[i];
        if (ch == '/') {
            if (c != BOARD_SIZE) return false;
            ++r;
            c = 0;
            ++i;
            continue;
        }
        if (ch == ' ') break; // 进入 turn 部分
        if (std::isdigit(ch)) {
            int empty = ch - '0';
            c += empty;
            ++i;
            continue;
        }
        int idx = pos(r, c);
        if (ch == 'w' || ch == 'W') {
            s.wolf_bb |= (1u << idx);
        } else if (ch == 's' || ch == 'S') {
            s.sheep_bb |= (1u << idx);
        } else {
            return false;
        }
        ++c;
        ++i;
    }

    // 解析 turn
    while (i < fen.size() && fen[i] == ' ') ++i;
    if (i < fen.size()) {
        s.turn = (fen[i] == 'w' || fen[i] == 'W');
        ++i;
    }

    // 解析 move_count（可选）
    while (i < fen.size() && fen[i] == ' ') ++i;
    if (i < fen.size() && std::isdigit(fen[i])) {
        s.move_count = std::stoi(fen.substr(i));
    }

    return true;
}

// 输出一个走法为 JSON
void print_move_json(const Move& m) {
    int fr = row_of(m.from), fc = col_of(m.from);
    int tr = row_of(m.to), tc = col_of(m.to);
    std::cout << "{\"from\":[" << fr << "," << fc << "]";
    std::cout << ",\"to\":[" << tr << "," << tc << "]";
    if (m.captured >= 0) {
        int cr = row_of(m.captured), cc = col_of(m.captured);
        std::cout << ",\"captured\":[" << cr << "," << cc << "]";
    } else {
        std::cout << ",\"captured\":null";
    }
    std::cout << "}";
}

// 排序走法用于比较（按 from, to, captured 排序）
bool move_less(const Move& a, const Move& b) {
    if (a.from != b.from) return a.from < b.from;
    if (a.to != b.to) return a.to < b.to;
    return a.captured < b.captured;
}

void process_fen(const std::string& fen) {
    State s;
    if (!parse_fen(fen, s)) {
        std::cout << "{\"fen\":\"" << fen << "\",\"error\":\"parse failed\"}\n";
        return;
    }

    auto moves = gen_moves(s);
    std::sort(moves.begin(), moves.end(), move_less);

    std::cout << "{\"fen\":\"" << fen << "\",\"moves\":[";
    for (size_t i = 0; i < moves.size(); ++i) {
        if (i > 0) std::cout << ",";
        print_move_json(moves[i]);
    }
    std::cout << "],\"terminal\":";
    Result r = is_terminal(s);
    if (r == Result::WOLF_WIN) std::cout << "\"wolf_win\"";
    else if (r == Result::SHEEP_WIN) std::cout << "\"sheep_win\"";
    else if (r == Result::DRAW) std::cout << "\"draw\"";
    else std::cout << "null";
    std::cout << "}\n";
    std::cout.flush();
}

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        // 跳过空白行和注释
        if (line.empty() || line[0] == '#') continue;
        process_fen(line);
    }
    return 0;
}