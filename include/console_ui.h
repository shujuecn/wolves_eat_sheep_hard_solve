#pragma once
// 终端控制台 UI 共享工具（header-only，无第三方依赖）
#include "board.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace wolves::conui {

// ---- ANSI 颜色（仅 TTY 时启用）----
inline bool g_color = false;
inline const char* reset()  { return g_color ? "\033[0m" : ""; }
inline const char* red()    { return g_color ? "\033[91m" : ""; }
inline const char* cyan()   { return g_color ? "\033[96m" : ""; }
inline const char* gold()   { return g_color ? "\033[93m" : ""; }
inline const char* green()  { return g_color ? "\033[92m" : ""; }
inline const char* gray()   { return g_color ? "\033[90m" : ""; }
inline const char* bold()   { return g_color ? "\033[1m" : ""; }
inline const char* inverse(){ return g_color ? "\033[7m" : ""; }

// ---- 坐标 <-> 字符串 ----
// 支持：a1..e5（列 a-e，行 1-5 自上而下）；r<行>c<列>；<行列>（如 13 = 行1列3）
inline bool parse_cell(const std::string& tok, int& row, int& col) {
    std::string s;
    for (char c : tok) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.size() == 2 && s[0] >= 'a' && s[0] <= 'e' && s[1] >= '1' && s[1] <= '5') {
        col = s[0] - 'a';
        row = s[1] - '1';
        return true;
    }
    if (s.size() >= 3 && s[0] == 'r') {
        size_t p = s.find('c');
        if (p != std::string::npos) {
            try {
                row = std::stoi(s.substr(1, p - 1)) - 1;
                col = std::stoi(s.substr(p + 1)) - 1;
            } catch (...) { return false; }
            return row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE;
        }
    }
    if (s.size() == 2 && s[0] >= '1' && s[0] <= '5' && s[1] >= '1' && s[1] <= '5') {
        row = s[0] - '1';
        col = s[1] - '1';
        return true;
    }
    return false;
}

inline std::string cell_name(int row, int col) {
    std::string s;
    s += static_cast<char>('a' + col);
    s += static_cast<char>('1' + row);
    return s;
}

inline std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---- 棋盘渲染 ----
// flip=true 表示 180° 旋转显示（仅影响视角，不影响逻辑坐标）。
// selected: 选中格（-1 无）；targets: 合法落点（渲染为金色）。
inline std::string render_board(const State& s, bool flip,
                                int selected = -1,
                                const std::vector<int>& targets = {}) {
    std::string out;
    auto in_target = [&](int r, int c) {
        for (int t : targets) if (t == pos(r, c)) return true;
        return false;
    };

    // 顶部列标（按视觉方向）
    out += "     ";
    for (int vc = 0; vc < BOARD_SIZE; ++vc) {
        int gc = flip ? BOARD_SIZE - 1 - vc : vc;
        out += " ";
        out += static_cast<char>('a' + gc);
        out += "  ";
    }
    out += "\n";
    out += "   ┌───┬───┬───┬───┬───┐\n";
    for (int vr = 0; vr < BOARD_SIZE; ++vr) {
        int gr = flip ? BOARD_SIZE - 1 - vr : vr;
        out += " ";
        out += static_cast<char>('1' + gr);
        out += " │";
        for (int vc = 0; vc < BOARD_SIZE; ++vc) {
            int gc = flip ? BOARD_SIZE - 1 - vc : vc;
            Piece p = piece_at(s, pos(gr, gc));
            bool tgt = in_target(gr, gc);
            bool sel = (pos(gr, gc) == selected);
            const char* ch = p == Piece::WOLF ? "W" : p == Piece::SHEEP ? "S" : "·";
            std::string cell;
            if (sel) {
                cell = " " + std::string(inverse()) + ch + reset() + " ";
            } else if (tgt) {
                cell = " " + std::string(gold()) + ch + reset() + " ";
            } else if (p == Piece::WOLF) {
                cell = " " + std::string(red()) + "W" + reset() + " ";
            } else if (p == Piece::SHEEP) {
                cell = " " + std::string(cyan()) + "S" + reset() + " ";
            } else {
                cell = " · ";
            }
            out += cell;
            out += "│";
        }
        out += "\n";
        if (vr < BOARD_SIZE - 1) out += "   ├───┼───┼───┼───┼───┤\n";
    }
    out += "   └───┴───┴───┴───┴───┘\n";
    if (flip) out += "   （视图已旋转 180°：a5 在左下）\n";
    return out;
}

inline std::string piece_label(Piece p) {
    return p == Piece::WOLF ? std::string(red()) + "狼" + reset() :
           p == Piece::SHEEP ? std::string(cyan()) + "羊" + reset() : "·";
}

} // namespace wolves::conui