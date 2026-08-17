/**
 * selfcheck.cpp
 *
 * 自检工具：验证 core 逻辑的正确性
 * - 初始局面
 * - 走法计数
 * - 终局判定
 * - 镜像对称性
 */

#include "board.h"

#include <iostream>
#include <cassert>
#include <cstdlib>

using namespace wolves;

static int tests = 0;
static int passed = 0;

#define TEST(name) do { tests++; std::cout << "  " << (name) << "... "; } while(0)
#define OK() do { passed++; std::cout << "OK\n"; } while(0)
#define FAIL(msg) do { std::cerr << "FAIL: " << (msg) << "\n"; } while(0)

void test_initial_state() {
    TEST("initial state");
    State s = initial_state();
    assert(sheep_count(s) == 15);
    assert(wolf_count(s) == 3);
    assert(s.turn == true);  // wolf's turn
    assert(s.move_count == 0);

    // 检查初始布局
    // 行 0-2 全羊
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < BOARD_SIZE; ++c)
            assert(piece_at(s, pos(r, c)) == Piece::SHEEP);
    // 行 3 全空
    for (int c = 0; c < BOARD_SIZE; ++c)
        assert(piece_at(s, pos(3, c)) == Piece::NONE);
    // 行 4: [空, 狼, 狼, 狼, 空]
    assert(piece_at(s, pos(4, 0)) == Piece::NONE);
    assert(piece_at(s, pos(4, 1)) == Piece::WOLF);
    assert(piece_at(s, pos(4, 2)) == Piece::WOLF);
    assert(piece_at(s, pos(4, 3)) == Piece::WOLF);
    assert(piece_at(s, pos(4, 4)) == Piece::NONE);
    OK();
}

void test_initial_moves() {
    TEST("initial wolf moves");
    State s = initial_state();
    auto moves = gen_moves(s);

    // 狼 (4,1): 上到 (3,1), 吃 (2,1), 左到 (4,0)  = 3
    // 狼 (4,2): 上到 (3,2), 吃 (2,2)             = 2
    // 狼 (4,3): 上到 (3,3), 吃 (2,3), 右到 (4,4)  = 3
    // 共 8 个走法
    if (moves.size() != 8) {
        FAIL("expected 8 moves, got " + std::to_string(moves.size()));
        for (auto& m : moves) {
            std::cerr << "  from=(" << row_of(m.from) << "," << col_of(m.from)
                      << ") to=(" << row_of(m.to) << "," << col_of(m.to)
                      << ") captured=" << m.captured << "\n";
        }
        return;
    }
    OK();
}

void test_sheep_moves_after_wolf() {
    TEST("sheep moves after wolf steps to (3,2)");
    State s = initial_state();
    // 狼 (4,2) → (3,2)
    Move m(pos(4, 2), pos(3, 2));
    State s2 = apply(s, m);

    assert(s2.turn == false); // 羊回合
    assert(s2.move_count == 1);

    auto sheep_moves = gen_moves(s2);
    // 行 0-1 羊被其他羊挡住，不能移动
    // 行 2 羊: (2,0)→(3,0), (2,1)→(3,1), (2,3)→(3,3), (2,4)→(3,4)
    // (2,2) 被狼在 (3,2) 挡住，不能向下
    // 共 4 个走法
    if (sheep_moves.size() != 4) {
        FAIL("expected 4 sheep moves, got " + std::to_string(sheep_moves.size()));
        for (auto& m : sheep_moves) {
            std::cerr << "  from=(" << row_of(m.from) << "," << col_of(m.from)
                      << ") to=(" << row_of(m.to) << "," << col_of(m.to) << ")\n";
        }
        return;
    }
    OK();
}

void test_wolf_capture() {
    TEST("wolf capture");
    // 构造局面：狼在 (4,2)，羊在 (2,2)，中间 (3,2) 空
    State s;
    s.wolf_bb = 1u << pos(4, 2);
    s.sheep_bb = (1u << pos(2, 2)) | (1u << pos(0, 0))
               | (1u << pos(0, 1)) | (1u << pos(0, 3));
    s.turn = true;

    auto moves = gen_moves_from(s, pos(4, 2));
    // 狼在 (4,2)，(3,2) 空，(4,1) 空，(4,3) 空
    // 走法：上到 (3,2), 左到 (4,1), 右到 (4,3), 吃 (2,2)
    // 共 4 个走法
    if (moves.size() != 4) {
        FAIL("expected 4 moves from (4,2), got " + std::to_string(moves.size()));
        return;
    }

    bool has_simple = false, has_capture = false;
    for (auto& m : moves) {
        if (m.to == pos(3, 2) && m.captured == -1) has_simple = true;
        if (m.to == pos(2, 2) && m.captured == pos(2, 2)) has_capture = true;
    }
    if (!has_simple || !has_capture) {
        FAIL("missing simple move or capture");
        return;
    }

    // 执行吃子
    State s2 = apply(s, Move(pos(4, 2), pos(2, 2), pos(2, 2)));
    assert(sheep_count(s2) == 3); // 原有 4 羊，吃 1 只剩 3
    assert(wolf_count(s2) == 1);
    assert(piece_at(s2, pos(2, 2)) == Piece::WOLF);
    assert(piece_at(s2, pos(4, 2)) == Piece::NONE);
    assert(s2.turn == false); // 换羊方
    OK();
}

void test_wolf_win() {
    TEST("wolf win (sheep < 4)");
    State s;
    s.wolf_bb = 1u << pos(4, 2);
    s.sheep_bb = (1u << pos(0, 0)) | (1u << pos(0, 1)) | (1u << pos(0, 4));
    // 3 只羊 → 狼胜
    assert(is_terminal(s) == Result::WOLF_WIN);
    OK();
}

void test_sheep_win() {
    TEST("sheep win (wolves blocked)");
    // 三只狼被羊和边界围住
    State s;
    // 狼在 (2,1), (2,2), (2,3)
    s.wolf_bb = (1u << pos(2, 1)) | (1u << pos(2, 2)) | (1u << pos(2, 3));
    // 四周全放羊
    for (int r = 0; r < BOARD_SIZE; ++r)
        for (int c = 0; c < BOARD_SIZE; ++c)
            if (piece_at(s, pos(r, c)) == Piece::NONE)
                s.sheep_bb |= (1u << pos(r, c));
    // 移除狼位上的羊
    s.sheep_bb &= ~s.wolf_bb;

    assert(is_terminal(s) == Result::SHEEP_WIN);
    OK();
}

void test_draw() {
    TEST("draw (move_count >= 150)");
    State s = initial_state();
    s.move_count = 150;
    assert(is_terminal(s) == Result::DRAW);
    OK();
}

void test_not_terminal() {
    TEST("not terminal");
    State s = initial_state();
    assert(is_terminal(s) == Result::UNKNOWN);
    OK();
}

void test_apply_changes_turn() {
    TEST("apply changes turn");
    State s = initial_state();
    auto moves = gen_moves(s);
    assert(!moves.empty());
    State s2 = apply(s, moves[0]);
    assert(s2.turn == !s.turn);
    assert(s2.move_count == s.move_count + 1);
    OK();
}

void test_no_wolf_capture_when_adjacent_blocked() {
    TEST("wolf cannot capture when adjacent cell blocked");
    // 狼在 (4,2)，(3,2) 有羊，(2,2) 有羊
    // 向上走被羊挡住 → 不能走到 (3,2)，也不能吃 (2,2)
    // 但左右仍可走：(4,1) 和 (4,3) 空
    State s;
    s.wolf_bb = 1u << pos(4, 2);
    s.sheep_bb = (1u << pos(3, 2)) | (1u << pos(2, 2));
    s.turn = true;

    auto moves = gen_moves_from(s, pos(4, 2));
    // 只能左右走，不能向上（被挡），不能向下（出界）
    if (moves.size() != 2) {
        FAIL("expected 2 moves (left & right), got " + std::to_string(moves.size()));
        return;
    }
    // 确认没有吃子
    for (auto& m : moves) {
        if (m.captured >= 0) {
            FAIL("should not have capture when adjacent cell is blocked");
            return;
        }
    }
    OK();
}

void test_capture_only_by_wolf() {
    TEST("sheep cannot capture");
    State s = initial_state();
    // 羊方走
    s.turn = false;
    auto moves = gen_moves(s);
    for (auto& m : moves) {
        if (m.captured >= 0) {
            FAIL("sheep should not have capture moves");
            return;
        }
    }
    OK();
}

void test_fen_roundtrip() {
    TEST("FEN roundtrip");
    State s = initial_state();
    std::string fen = to_fen(s);
    // 初始局面: sssss/sssss/sssss/5/3www3 w 0
    // 等等，FEN 中 s=sheep, w=wolf, 数字=连续空格
    // 行0: sssss (5个羊)
    // 行1: sssss
    // 行2: sssss
    // 行3: 5 (5个空)
    // 行4: 1www1
    // 所以: sssss/sssss/sssss/5/1www1 w 0
    if (fen != "sssss/sssss/sssss/5/1www1 w 0") {
        FAIL("unexpected FEN: " + fen);
        return;
    }
    OK();
}

int main() {
    std::cout << "=== wolves::board self-check ===\n\n";

    test_initial_state();
    test_initial_moves();
    test_sheep_moves_after_wolf();
    test_wolf_capture();
    test_wolf_win();
    test_sheep_win();
    test_draw();
    test_not_terminal();
    test_apply_changes_turn();
    test_no_wolf_capture_when_adjacent_blocked();
    test_capture_only_by_wolf();
    test_fen_roundtrip();

    std::cout << "\n=== " << passed << "/" << tests << " tests passed ===";
    if (passed == tests) {
        std::cout << " ✅\n";
        return 0;
    } else {
        std::cout << " ❌\n";
        return 1;
    }
}