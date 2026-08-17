#include "board.h"

#include <algorithm>
#include <bit>
#include <sstream>

namespace wolves {

// ---- 工厂函数 ----

State initial_state() {
    State s;
    // 行 0-2 全放羊
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            s.sheep_bb |= (1u << pos(r, c));
        }
    }
    // 行 4 列 1,2,3 放狼
    s.wolf_bb = (1u << pos(4, 1)) | (1u << pos(4, 2)) | (1u << pos(4, 3));
    s.turn = true;   // 狼先手
    s.move_count = 0;
    return s;
}

// ---- 局面查询 ----

Piece piece_at(const State& s, int idx) {
    uint32_t mask = 1u << idx;
    if (s.wolf_bb & mask) return Piece::WOLF;
    if (s.sheep_bb & mask) return Piece::SHEEP;
    return Piece::NONE;
}

int sheep_count(const State& s) {
    return std::popcount(s.sheep_bb);
}

int wolf_count(const State& s) {
    return std::popcount(s.wolf_bb);
}

uint32_t occupied(const State& s) {
    return s.sheep_bb | s.wolf_bb;
}

// ---- 走法生成 ----

std::vector<Move> gen_moves_from(const State& s, int idx) {
    std::vector<Move> moves;
    Piece p = piece_at(s, idx);
    if (p == Piece::NONE) return moves;

    int r = row_of(idx);
    int c = col_of(idx);
    uint32_t occ = occupied(s);

    for (const auto& d : DIRS) {
        int nr = r + d[0];
        int nc = c + d[1];
        if (!in_bounds(nr, nc)) continue;

        int next_idx = pos(nr, nc);
        // 相邻格被占 → 不可走
        if (occ & (1u << next_idx)) continue;

        // 简单移动
        moves.emplace_back(idx, next_idx);

        // 吃子：只有狼能吃，隔一格为羊
        if (p == Piece::WOLF) {
            int prey_r = r + 2 * d[0];
            int prey_c = c + 2 * d[1];
            if (!in_bounds(prey_r, prey_c)) continue;

            int prey_idx = pos(prey_r, prey_c);
            if (s.sheep_bb & (1u << prey_idx)) {
                moves.emplace_back(idx, prey_idx, prey_idx);
            }
        }
    }
    return moves;
}

std::vector<Move> gen_moves(const State& s) {
    std::vector<Move> moves;
    uint32_t pieces = s.turn ? s.wolf_bb : s.sheep_bb;

    // 遍历当前回合方的所有棋子
    while (pieces) {
        int idx = std::countr_zero(pieces);
        pieces &= pieces - 1; // 清除最低位

        auto from_moves = gen_moves_from(s, idx);
        moves.insert(moves.end(), from_moves.begin(), from_moves.end());
    }
    return moves;
}

bool can_move(const State& s, int idx) {
    Piece p = piece_at(s, idx);
    if (p == Piece::NONE) return false;

    int r = row_of(idx);
    int c = col_of(idx);
    uint32_t occ = occupied(s);

    for (const auto& d : DIRS) {
        int nr = r + d[0];
        int nc = c + d[1];
        if (!in_bounds(nr, nc)) continue;
        if (!(occ & (1u << pos(nr, nc)))) return true;
    }
    return false;
}

bool any_wolf_can_move(const State& s) {
    uint32_t wolves = s.wolf_bb;
    while (wolves) {
        int idx = std::countr_zero(wolves);
        wolves &= wolves - 1;
        if (can_move(s, idx)) return true;
    }
    return false;
}

// ---- 局面操作 ----

State apply(const State& s, const Move& m) {
    State next = s;
    uint32_t from_mask = 1u << m.from;
    uint32_t to_mask = 1u << m.to;

    if (s.turn) {
        // 狼移动
        next.wolf_bb = (s.wolf_bb & ~from_mask) | to_mask;
        if (m.captured >= 0) {
            next.sheep_bb &= ~(1u << m.captured);
        }
    } else {
        // 羊移动
        next.sheep_bb = (s.sheep_bb & ~from_mask) | to_mask;
    }

    next.turn = !s.turn;
    next.move_count = s.move_count + 1;
    return next;
}

// ---- 终局判定 ----

Result is_terminal(const State& s) {
    // 羊 < 4 → 狼胜
    if (sheep_count(s) < 4) {
        return Result::WOLF_WIN;
    }
    // 三狼无合法移动 → 羊胜
    if (!any_wolf_can_move(s)) {
        return Result::SHEEP_WIN;
    }
    // 150 步耗尽 → 平局
    if (s.move_count >= MAX_MOVES) {
        return Result::DRAW;
    }
    return Result::UNKNOWN;
}

// ---- 调试 ----

std::string to_string(const State& s) {
    std::ostringstream oss;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int idx = pos(r, c);
            Piece p = piece_at(s, idx);
            if (p == Piece::WOLF) oss << 'W';
            else if (p == Piece::SHEEP) oss << 'S';
            else oss << '.';
            if (c < BOARD_SIZE - 1) oss << ' ';
        }
        oss << '\n';
    }
    oss << "Turn: " << (s.turn ? "WOLF" : "SHEEP");
    oss << "  Moves: " << s.move_count;
    return oss.str();
}

std::string to_fen(const State& s) {
    // FEN 格式：5×5，按行输出，每行用数字表示连续空格
    // 行间用 '/' 分隔，后接空格 + "w"/"s" + 空格 + move_count
    std::ostringstream oss;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        int empty = 0;
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int idx = pos(r, c);
            Piece p = piece_at(s, idx);
            if (p == Piece::NONE) {
                ++empty;
            } else {
                if (empty > 0) {
                    oss << empty;
                    empty = 0;
                }
                oss << (p == Piece::WOLF ? 'w' : 's');
            }
        }
        if (empty > 0) oss << empty;
        if (r < BOARD_SIZE - 1) oss << '/';
    }
    oss << ' ' << (s.turn ? 'w' : 's');
    oss << ' ' << s.move_count;
    return oss.str();
}

} // namespace wolves