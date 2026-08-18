// play.cpp — 狼羊棋终端复刻版（C++，对应 Python 版 game.py 的双人对局）
//
// 用法：./build/play [--no-color]
// 交互：输入 a1..e5 坐标；单格=选中棋子（显示合法落点），再输目标格落子；
//       或一次输入两格 "a1 b2" 直接走子。命令：u=悔棋 r=旋转180° b=重绘棋盘
//       h=帮助 q=退出。
// 规则与 Python rules.py 一致：
//   - 狼先行，每回合一个棋子上下左右移动一格；狼可跳过一格空格吃掉
//     前方两格的羊（每回合至多一只，吃后立即换羊方）。
//   - 羊 < 4 → 狼胜；三狼全无合法移动 → 羊胜。
//   - 双方合计 150 步 → 平局；双方最近一步都是同一棋子连续往返 >= 5 次 → 平局。
#include "board.h"
#include "console_ui.h"

#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

using namespace wolves;
using namespace wolves::conui;

namespace {

constexpr int IDLE_LIMIT = 5;

// ---------- 闲步计数（与 rules.py 一致） ----------
struct Game {
    State s;
    // 闲步规则状态
    std::unordered_map<int, std::string> piece_ids;      // 格 -> 棋子 id
    std::unordered_map<std::string, std::deque<int>> histories;  // id -> 位置历史
    std::map<std::string, std::string> last_piece_by_side;       // 侧 -> id
    std::map<std::string, int> idle_streaks;                      // 侧 -> 当前往返计数
    std::string winner_reason;  // 终局说明（空=未终局）

    void sync_piece_ids() {
        std::map<int, Piece> occ;
        for (int i = 0; i < 25; ++i) {
            Piece p = piece_at(s, i);
            if (p != Piece::NONE) occ[i] = p;
        }
        std::map<int, Piece> cur;
        for (const auto& kv : piece_ids) {
            cur[kv.first] = piece_at(s, kv.first);
        }
        if (cur == occ) return;
        piece_ids.clear();
        histories.clear();
        last_piece_by_side.clear();
        for (const auto& kv : occ) {
            std::string id = (kv.second == Piece::WOLF ? "wolf:" : "sheep:") +
                             std::to_string(kv.first);
            piece_ids[kv.first] = id;
            histories[id] = std::deque<int>(1, kv.first);
        }
    }

    static int back_and_forth_count(const std::deque<int>& history) {
        if (history.size() < 3) return 0;
        int count = 1;
        for (int index = static_cast<int>(history.size()) - 3; index >= 0; --index) {
            if (history[index] != history[index + 2]) break;
            ++count;
        }
        return count >= 2 ? count : 0;
    }

    // 执行一步，返回人读描述；非法返回空串
    std::string make_move(const Move& m) {
        if (s.turn != (piece_at(s, m.from) == Piece::WOLF)) return "";
        bool legal = false;
        for (const Move& cand : gen_moves_from(s, m.from)) {
            if (cand.to == m.to && cand.captured == m.captured) { legal = true; break; }
        }
        if (!legal) return "";

        sync_piece_ids();
        Piece piece = piece_at(s, m.from);
        std::string side = (piece == Piece::WOLF) ? "wolf" : "sheep";
        std::string piece_id = piece_ids[m.from];
        auto& history = histories[piece_id];
        if (last_piece_by_side[side] != piece_id) {
            history.clear();
            history.push_back(m.from);
        }

        std::string desc = cell_name(row_of(m.from), col_of(m.from)) + "→" +
                           cell_name(row_of(m.to), col_of(m.to));
        if (m.captured >= 0) {
            desc += "（吃 " + cell_name(row_of(m.captured), col_of(m.captured)) + " 羊）";
        }

        s = apply(s, m);  // apply 翻转回合并递增 move_count

        piece_ids.erase(m.from);
        if (m.captured >= 0) {
            auto it = piece_ids.find(m.captured);
            if (it != piece_ids.end()) {
                histories.erase(it->second);
                piece_ids.erase(it);
            }
        }
        piece_ids[m.to] = piece_id;
        history.push_back(m.to);
        last_piece_by_side[side] = piece_id;

        int repeat = back_and_forth_count(history);
        idle_streaks[side] = repeat;

        check_winner();
        if (winner_reason.empty() &&
            idle_streaks["wolf"] >= IDLE_LIMIT && idle_streaks["sheep"] >= IDLE_LIMIT) {
            winner_reason = "双方最近一步均为同一棋子连续往返 ≥ " +
                            std::to_string(IDLE_LIMIT) + " 次";
        }
        if (winner_reason.empty() && s.move_count >= MAX_MOVES) {
            winner_reason = std::to_string(MAX_MOVES) + " 步已用尽";
        }
        return desc;
    }

    void check_winner() {
        Result r = is_terminal(s);
        if (r == Result::WOLF_WIN) winner_reason = "羊已不足 4 只";
        else if (r == Result::SHEEP_WIN) winner_reason = "3 只狼均无法移动";
        else if (r == Result::DRAW) winner_reason = std::to_string(MAX_MOVES) + " 步已用尽";
    }
};

struct Snapshot {
    State s;
    decltype(Game::piece_ids) piece_ids;
    decltype(Game::histories) histories;
    decltype(Game::last_piece_by_side) last_piece_by_side;
    decltype(Game::idle_streaks) idle_streaks;
    std::string winner_reason;
};

std::string status_line(const Game& g) {
    if (!g.winner_reason.empty()) {
        std::string winner;
        if (g.winner_reason.find("不足 4") != std::string::npos) winner = "狼方胜";
        else if (g.winner_reason.find("无法移动") != std::string::npos) winner = "羊方胜";
        else winner = "平局";
        return winner + " — " + g.winner_reason;
    }
    return std::string(g.s.turn ? "狼方回合" : "羊方回合") + "（" +
           (g.s.turn ? std::string(red()) + "红狼行动" + reset()
                     : std::string(cyan()) + "黑羊行动" + reset()) + "） 第 " +
           std::to_string(g.s.move_count + 1) + " 步";
}

void print_help() {
    std::cout << bold() << "狼羊棋·终端版" << reset() << "（C++ 复刻 Python GUI 双人对局）\n"
              << "  坐标：a1..e5（列 a-e × 行 1-5，行 1 在上方）；狼 W(红)，羊 S(青)\n"
              << "  走子：输入一格格子名选中（合法落点金色高亮），再输入目标格；\n"
              << "        或直接两格：例如  " << gold() << "b3 c3" << reset() << "\n"
              << "  命令：u=悔棋  r=旋转180°  b=重绘  h=帮助  q=退出\n";
}

} // namespace

int main(int argc, char** argv) {
    conui::g_color = isatty(STDOUT_FILENO);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) conui::g_color = false;
        else { std::cerr << "未知参数: " << argv[i] << "\n"; return 1; }
    }

    Game g;
    g.s = initial_state();
    bool flip = false;
    std::vector<Snapshot> undo_stack;
    int selected = -1;
    std::vector<int> targets;

    print_help();
    std::cout << render_board(g.s, flip) << status_line(g) << "\n";

    std::string line;
    while (true) {
        std::cout << green() << "狼羊棋> " << reset();
        if (!std::getline(std::cin, line)) break;
        auto toks = tokenize(line);
        if (toks.empty()) continue;
        const std::string& cmd = toks[0];

        auto do_undo = [&]() {
            if (undo_stack.empty()) { std::cout << gray() << "（没有可悔的棋）" << reset() << "\n"; return; }
            Snapshot snap = std::move(undo_stack.back());
            undo_stack.pop_back();
            g.s = snap.s;
            g.piece_ids = std::move(snap.piece_ids);
            g.histories = std::move(snap.histories);
            g.last_piece_by_side = std::move(snap.last_piece_by_side);
            g.idle_streaks = std::move(snap.idle_streaks);
            g.winner_reason = snap.winner_reason;
            selected = -1; targets.clear();
            std::cout << render_board(g.s, flip) << status_line(g) << "\n";
        };

        if (cmd == "q" || cmd == "quit" || cmd == "exit") break;
        if (cmd == "h" || cmd == "help" || cmd == "?") { print_help(); continue; }
        if (cmd == "b") { std::cout << render_board(g.s, flip) << status_line(g) << "\n"; continue; }
        if (cmd == "r") { flip = !flip; std::cout << render_board(g.s, flip) << status_line(g) << "\n"; continue; }
        if (cmd == "u") { do_undo(); continue; }

        if (!g.winner_reason.empty()) {
            std::cout << gold() << "对局已结束（" << g.winner_reason << "），输入 'n' 无效；请重启程序或 'q' 退出。"
                      << reset() << "\n";
            continue;
        }

        // 走子：一个格子=选中；两个格子=起止
        int r1, c1, r2, c2;
        if (toks.size() == 1 && parse_cell(cmd, r1, c1)) {
            int idx = pos(r1, c1);
            Piece p = piece_at(g.s, idx);
            if (p == Piece::NONE) { std::cout << gray() << "（该格无棋子）" << reset() << "\n"; continue; }
            if ((p == Piece::WOLF) != g.s.turn) { std::cout << gray() << "（不是当前回合方的棋子）" << reset() << "\n"; continue; }
            selected = idx;
            targets.clear();
            for (const Move& m : gen_moves_from(g.s, idx)) targets.push_back(m.to);
            std::cout << render_board(g.s, flip, selected, targets);
            if (targets.empty()) {
                std::cout << gray() << "（此棋子无合法走法）" << reset() << "\n";
                selected = -1;
            }
            continue;
        }
        if (toks.size() >= 2 && parse_cell(toks[0], r1, c1) && parse_cell(toks[1], r2, c2)) {
            // 校验：起格棋子必须属于当前回合方
            Piece pfrom = piece_at(g.s, pos(r1, c1));
            if (pfrom == Piece::NONE) {
                std::cout << gray() << "（非法走法：该格无棋子）" << reset() << "\n";
                continue;
            }
            if ((pfrom == Piece::WOLF) != g.s.turn) {
                std::cout << gray() << "（非法走法：不是当前回合方的棋子）" << reset() << "\n";
                continue;
            }
            // 查找合法走法（含吃子目标）
            Move chosen(-1, -1);
            for (const Move& m : gen_moves_from(g.s, pos(r1, c1))) {
                if (m.to == pos(r2, c2)) { chosen = m; break; }
            }
            if (chosen.from < 0) {
                std::cout << gray() << "（非法走法：" << toks[0] << " → " << toks[1] << "）" << reset() << "\n";
                continue;
            }
            Snapshot snap{g.s, g.piece_ids, g.histories, g.last_piece_by_side,
                          g.idle_streaks, g.winner_reason};
            undo_stack.push_back(std::move(snap));
            std::string desc = g.make_move(chosen);
            if (desc.empty()) {
                undo_stack.pop_back();  // 防御：make_move 拒绝时撤销压栈
                std::cout << gray() << "（非法走法：" << toks[0] << " → " << toks[1] << "）" << reset() << "\n";
                continue;
            }
            selected = -1; targets.clear();
            std::cout << render_board(g.s, flip) << status_line(g);
            if (!g.winner_reason.empty()) std::cout << "  ← 对局结束";
            std::cout << "\n";
            if (!g.winner_reason.empty()) {
                std::cout << gold() << status_line(g) << reset() << "\n";
            }
            continue;
        }
        std::cout << gray() << "（无法识别的输入，输入 h 查看帮助）" << reset() << "\n";
    }
    std::cout << "再见。\n";
    return 0;
}