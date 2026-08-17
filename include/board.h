#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wolves {

// ---- 常量 ----
constexpr int BOARD_SIZE = 5;
constexpr int MAX_MOVES = 150;
constexpr int TOTAL_CELLS = BOARD_SIZE * BOARD_SIZE; // 25

// 四个正交方向：上、下、左、右
constexpr int DIRS[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

// ---- 结果枚举 ----
enum class Result : uint8_t {
    WOLF_WIN = 0,   // 羊 < 4
    SHEEP_WIN = 1,  // 三狼无合法移动
    DRAW = 2,       // 150 步耗尽
    UNKNOWN = 3     // 未定
};

// ---- 棋子类型 ----
enum class Piece : uint8_t {
    NONE = 0,
    WOLF = 1,
    SHEEP = 2
};

// ---- 走法 ----
struct Move {
    int from;       // 起始格索引 0..24
    int to;         // 目标格索引 0..24
    int captured;   // 被吃子格索引，-1 表示无吃子

    Move(int f, int t, int c = -1) : from(f), to(t), captured(c) {}

    bool operator==(const Move& other) const {
        return from == other.from && to == other.to && captured == other.captured;
    }
    bool operator!=(const Move& other) const { return !(*this == other); }
};

// ---- 局面 ----
// 用 25 位位板表示，位 0 = (row=0,col=0)，位 24 = (row=4,col=4)
// sheep_bb 和 wolf_bb 互斥（同一格不能同时有羊和狼）
struct State {
    uint32_t sheep_bb;  // 羊位板
    uint32_t wolf_bb;   // 狼位板
    bool turn;          // true = 狼回合, false = 羊回合
    int move_count;     // 已走步数（双方合计）

    State() : sheep_bb(0), wolf_bb(0), turn(true), move_count(0) {}

    bool operator==(const State& other) const {
        return sheep_bb == other.sheep_bb
            && wolf_bb == other.wolf_bb
            && turn == other.turn;
    }
    bool operator!=(const State& other) const { return !(*this == other); }
};

// ---- 坐标转换 ----
inline constexpr int pos(int row, int col) { return row * BOARD_SIZE + col; }
inline constexpr int row_of(int idx) { return idx / BOARD_SIZE; }
inline constexpr int col_of(int idx) { return idx % BOARD_SIZE; }
inline constexpr bool in_bounds(int r, int c) {
    return r >= 0 && r < BOARD_SIZE && c >= 0 && c < BOARD_SIZE;
}

// ---- 工厂函数 ----
/// 创建初始局面：上三排 15 羊，底排中间三狼，狼先手
State initial_state();

// ---- 局面查询 ----
/// 返回某格上的棋子类型
Piece piece_at(const State& s, int idx);
/// 羊的数量
int sheep_count(const State& s);
/// 狼的数量
int wolf_count(const State& s);
/// 所有被占格子的位板
uint32_t occupied(const State& s);

// ---- 走法生成 ----
/// 从指定位置生成所有合法走法（含吃子）
std::vector<Move> gen_moves_from(const State& s, int idx);
/// 当前回合方的所有合法走法
std::vector<Move> gen_moves(const State& s);
/// 狼是否在某格有合法移动
bool can_move(const State& s, int idx);
/// 是否至少有一只狼能移动
bool any_wolf_can_move(const State& s);

// ---- 局面操作 ----
/// 执行一步走法，返回新状态（不检查合法性）
State apply(const State& s, const Move& m);

// ---- 终局判定 ----
/// 判定终局结果，非终局返回 UNKNOWN
Result is_terminal(const State& s);

// ---- 调试 ----
/// 转为可读字符串（人类视角：行 0 在上）
std::string to_string(const State& s);
/// 转为 FEN 类字符串（与 Python 兼容）
std::string to_fen(const State& s);

} // namespace wolves