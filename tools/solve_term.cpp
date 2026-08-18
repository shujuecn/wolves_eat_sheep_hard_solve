// solve_term.cpp — 狼羊棋终端解棋器（基于逆推表库）
//
// 用途：加载已求解的表库（data/tb_test/ 下 k=4..K 的完成桶），对任意局面给出
//   - 结局结论（狼胜/羊胜/和棋/未求解）与最快步数（DTM）
//   - 当前回合方的所有走法评估，★ 标记最优应手
//   - 最优完整路线（双方都按 DTM 最优）——pv 命令
//   - 对上一步的点评（是否最优、错失/保和/逆转）
//
// 交互命令：
//   m a1 b2      走子（当前回合方；也可用于复盘时交替输入双方走法）
//   ai / best    打印当前局面所有走法评估与最优应手
//   play         表库为当前回合方走出最优应手
//   pv [n]       打印最优路线（默认 20 个半回合）
//   setup [fen]  摆盘（无参进入交互摆盘，或直接给 FEN）
//   load <i>     载入预设局面；list 列出预设
//   undo         悔一步；pos 打印 FEN；rot 旋转显示；new 回到初始局面
//   refresh      重新扫描已求解桶；h 帮助；q 退出
//
// 用法：./build/solve_term [--data-dir data/tb_test] [--preset N]
//                        [--no-color] [--auto]
#include "board.h"
#include "console_ui.h"
#include "encode.h"
#include "tablebase.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace wolves;
using namespace wolves::conui;

namespace {

constexpr int K_MAX = 15;

// ---------- 表库只读加载（校验头 + 大小；绝不影响正在写文件的求解进程） ----------
struct LoadedTB {
    int k = -1;
    uint64_t entries = 0;
    const uint8_t* data = nullptr;  // 指向条目区（头部之后）
    int fd = -1;
    size_t map_len = 0;

    ~LoadedTB() {
        if (data) { ::munmap(const_cast<uint8_t*>(data) - 64, map_len); }
        if (fd >= 0) ::close(fd);
    }
    LoadedTB() = default;
    LoadedTB(const LoadedTB&) = delete;
    LoadedTB& operator=(const LoadedTB&) = delete;

    bool load(const std::string& dir, int kk) {
        std::string path = dir + "/wsf_tb_dtc_k" +
                           std::to_string(kk / 10) + std::to_string(kk % 10) + ".bin";
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (::fstat(fd, &st) != 0) { return false; }
        uint64_t expect = 64 + bucket_size(kk);
        if (static_cast<uint64_t>(st.st_size) != expect) return false;  // 未完成/被占用
        size_t len = static_cast<size_t>(st.st_size);
        uint8_t* map = static_cast<uint8_t*>(
            ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0));
        if (map == MAP_FAILED) return false;
        TBHeader hdr;
        std::memcpy(&hdr, map, sizeof(TBHeader));
        if (std::memcmp(hdr.magic, "WSTB", 4) != 0 || hdr.completed != 1 ||
            hdr.k != kk) {
            ::munmap(map, len);
            return false;
        }
        k = kk;
        entries = bucket_size(kk);
        data = map + 64;
        map_len = len;
        return true;
    }
};

class TbCache {
public:
    explicit TbCache(std::string dir) : dir_(std::move(dir)) {}
    ~TbCache() { tbs_.clear(); }  // LoadedTB 析构顺序：k 大的在前

    // 已求解桶列表（k 升序）
    std::vector<int> available() {
        std::vector<int> out;
        for (int k = 4; k <= K_MAX; ++k) {
            LoadedTB probe;
            if (probe.load(dir_, k)) out.push_back(k);
        }
        return out;
    }

    // 取条目；返回 nullptr 表示该桶未加载/未求解
    const uint8_t* entry(int k, uint64_t idx) {
        std::unique_ptr<LoadedTB>& tb = tbs_[k];
        if (!tb) {
            auto p = std::make_unique<LoadedTB>();
            if (!p->load(dir_, k)) return nullptr;
            tb = std::move(p);
        }
        if (idx >= tb->entries) return nullptr;
        return &tb->data[idx];
    }

private:
    std::string dir_;
    std::map<int, std::unique_ptr<LoadedTB>> tbs_;
};

// ---------- 局面 <-> 表库索引 ----------
std::vector<int> bb_positions(uint32_t bb) {
    std::vector<int> v;
    while (bb) {
        int i = __builtin_ctz(bb);
        v.push_back(i);
        bb &= bb - 1;
    }
    return v;
}

// 表库结果（绝对结局）相对"当前回合方"的立场
// board.h: State.turn = true 表示狼回合；encode.h: tb turn = true 表示羊回合
struct Verdict {
    bool known = false;
    uint8_t result = TB_UNKNOWN;  // TB_*
    uint8_t dist = 0;
    int k = 0;
    std::string label() const {
        const char* r = result == TB_WOLF_WIN  ? "狼胜" :
                        result == TB_SHEEP_WIN ? "羊胜" :
                        result == TB_DRAW      ? "和棋" : "未知";
        std::string s = r;
        if (result == TB_WOLF_WIN || result == TB_SHEEP_WIN)
            s += "（最快 " + std::to_string(dist) + " 步）";
        return s;
    }
};

Verdict lookup(TbCache& cache, const State& s) {
    int k = sheep_count(s);
    Verdict v;
    v.k = k;
    if (k < 4) {  // 理论终局：羊 < 4 → 狼胜
        v.known = true;
        v.result = TB_WOLF_WIN;
        v.dist = 0;
        return v;
    }
    uint32_t wr = encode_wolf_bb(s.wolf_bb);
    const auto& info = WOLF_INFO[wr];
    uint32_t sr = encode_sheep(info.free_list, bb_positions(s.sheep_bb), k);
    bool tb_turn = !s.turn;  // 编码约定：tb turn=true 表示羊回合
    uint64_t idx = state_index(wr, sr, k, tb_turn);
    const uint8_t* e = cache.entry(k, idx);
    if (!e) return v;
    v.known = true;
    v.result = tb_result(*e);
    v.dist = tb_distance(*e);
    return v;
}

// 走法评估
struct MoveEval {
    Move m;
    Verdict v;
    int succ_k;
    int rank;    // 对当前回合方：2=胜 1=和 0=败
    int dist;    // 排序用：胜取小、败取大
};

std::vector<MoveEval> evaluate_moves(TbCache& cache, const State& s) {
    std::vector<MoveEval> out;
    uint8_t my_win = s.turn ? TB_WOLF_WIN : TB_SHEEP_WIN;  // board: turn=true=狼
    for (const Move& m : gen_moves(s)) {
        State n = apply(s, m);
        Verdict v = lookup(cache, n);
        MoveEval e = {m, v, sheep_count(n), -1, 0};
        if (v.known) {
            e.rank = (v.result == my_win) ? 2 : (v.result == TB_DRAW ? 1 : 0);
            e.dist = v.dist;
        } else {
            e.rank = -1;  // 未知
            e.dist = 0;
        }
        out.push_back(e);
    }
    return out;
}

// 为当前回合方排序最优：胜(小距离) > 和 > 败(大距离)
bool move_better(const MoveEval& a, const MoveEval& b) {
    if (a.rank != b.rank) return a.rank > b.rank;
    if (a.rank == 2) return a.dist < b.dist;
    if (a.rank == 0) return a.dist > b.dist;
    return false;
}
bool move_optimal(const MoveEval& e, const std::vector<MoveEval>& all) {
    if (e.rank < 0) return false;
    for (const auto& o : all) {
        if (o.rank > e.rank) return false;
        if (o.rank == e.rank) {
            if (e.rank == 2 && o.dist < e.dist) return false;
            if (e.rank == 0 && o.dist > e.dist) return false;
        }
    }
    return true;
}

// ---------- FEN 解析（与 to_fen 互逆） ----------
bool parse_fen(const std::string& fen, State& s) {
    s = State();
    size_t sp = fen.find(' ');
    std::string board = fen.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : fen.substr(sp + 1);
    std::vector<std::string> rows;
    std::string cur;
    for (char c : board) {
        if (c == '/') { rows.push_back(cur); cur.clear(); }
        else cur += c;
    }
    rows.push_back(cur);
    if (rows.size() != BOARD_SIZE) return false;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        int c = 0;
        for (char ch : rows[r]) {
            if (ch >= '1' && ch <= '5') {
                c += ch - '0';
            } else if (ch == 'w' || ch == 'W') {
                if (c >= BOARD_SIZE) return false;
                s.wolf_bb |= (1u << pos(r, c));
                ++c;
            } else if (ch == 's' || ch == 'S') {
                if (c >= BOARD_SIZE) return false;
                s.sheep_bb |= (1u << pos(r, c));
                ++c;
            } else {
                return false;
            }
        }
        if (c != BOARD_SIZE) return false;
    }
    if (wolf_count(s) != 3 || sheep_count(s) < 4 || sheep_count(s) > 22) return false;
    // 回合与步数
    std::vector<std::string> toks;
    { std::string t; for (char ch : rest) {
        if (std::isspace(static_cast<unsigned char>(ch))) { if (!t.empty()) { toks.push_back(t); t.clear(); } }
        else t += ch; }
      if (!t.empty()) toks.push_back(t); }
    if (toks.empty()) return false;
    if (toks[0] == "w" || toks[0] == "s") s.turn = (toks[0] == "w");
    else return false;
    if (toks.size() >= 2) s.move_count = std::stoi(toks[1]);
    return true;
}

// ---------- 预设局面 ----------
struct Preset {
    const char* name;
    const char* fen;
};
// （k=4..9 的示例练习局面，均可用已求解表库直接解析）
const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"k6 羊方围堵·上", "www1s/ss1ss/1s3/5/5 s 0"},
        {"k6 羊方围堵·中", "www1s/1ss1s/ss3/5/5 s 0"},
        {"k6 羊方围堵·下", "www1s/ss1s1/2s2/3s1/5 s 0"},
        {"k5 狼速胜·4步",  "ww2w/5/sss1s/1s3/5 s 0"},
        {"k5 羊方受困",    "sww1s/ss1ws/5/5/5 s 0"},
        {"k5 狼胜·消耗战", "sswws/ss1ws/5/5/5 s 0"},
        {"k5 狼胜·围剿",   "w1w1w/5/sss2/2ss1/5 s 0"},
        {"k4 收尾·2步行",  "w2ww/5/s1ss1/2s2/5 s 0"},
    };
    return p;
}

// ---------- 输出 ----------
std::string turn_label(const State& s) {
    return std::string(s.turn ? "狼方" : "羊方");
}

void print_board(const State& s, bool flip, int selected = -1,
                 const std::vector<int>& targets = {}) {
    std::cout << render_board(s, flip, selected, targets);
}

void print_position(const State& s) {
    std::cout << "  局面: k=" << sheep_count(s) << "（羊数） 轮到: "
              << (s.turn ? std::string(red()) + "狼方" + reset()
                         : std::string(cyan()) + "羊方" + reset())
              << "  已走 " << s.move_count << " 步\n";
}

void print_verdict_line(const Verdict& v) {
    if (!v.known) {
        std::cout << gray() << "  表库: k=" << v.k << " 尚未求解（"
                  << (v.k <= 3 ? "羊<4 平凡狼胜" : "等待求解程序产出该桶") << "）"
                  << reset() << "\n";
        return;
    }
    const char* col = v.result == TB_WOLF_WIN ? red() :
                        v.result == TB_SHEEP_WIN ? cyan() : gray();
    std::cout << "  表库结论: " << col << v.label() << reset() << "\n";
}

std::string move_desc(const Move& m) {
    std::string d = cell_name(row_of(m.from), col_of(m.from)) + "→" +
                    cell_name(row_of(m.to), col_of(m.to));
    if (m.captured >= 0) d += "×" + cell_name(row_of(m.captured), col_of(m.captured));
    return d;
}

void print_moves_eval(TbCache& cache, const State& s, const char* prefix = "  ") {
    auto evals = evaluate_moves(cache, s);
    if (evals.empty()) { std::cout << prefix << gray() << "（无合法走法）" << reset() << "\n"; return; }
    std::sort(evals.begin(), evals.end(), move_better);
    std::cout << prefix << "所有走法评估（" << turn_label(s) << "）:\n";
    for (const auto& e : evals) {
        bool opt = move_optimal(e, evals);
        std::cout << prefix << "  " << (opt ? std::string(gold()) + "★ " : "  ")
                  << move_desc(e.m);
        if (e.rank < 0) std::cout << gray() << "  → (后继 k=" << e.succ_k << " 未求解)" << reset();
        else std::cout << "  → " << (e.rank == 2 ? green() : e.rank == 1 ? gold() : red())
                       << e.v.label() << reset();
        if (opt) std::cout << " " << gold() << "← 最优" << reset();
        std::cout << "\n";
    }
    if (evals[0].rank == 2) {
        std::cout << prefix << green() << "结论: " << turn_label(s) << "可强制获胜"
                  << reset() << "\n";
    } else if (evals[0].rank == 1) {
        std::cout << prefix << gold() << "结论: " << turn_label(s) << "可保和（无强制胜）"
                  << reset() << "\n";
    } else if (evals[0].rank == 0) {
        std::cout << prefix << red() << "结论: " << turn_label(s) << "必败（最长可抵抗 "
                  << evals[0].dist << " 步）" << reset() << "\n";
    }
}

// 上一步点评：before 为走子前局面的评估，m 为走的棋
void critique(TbCache& cache, const State& after, const Move& m,
              const std::vector<MoveEval>& before, const std::string& by) {
    if (before.empty() || evaluate_moves(cache, after).empty()) return;
    const MoveEval* best_before = &before[0];
    const MoveEval* cur = nullptr;
    for (const auto& e : before) if (e.m.from == m.from && e.m.to == m.to) cur = &e;
    if (!cur) return;
    bool was_opt = move_optimal(*cur, before);

    if (was_opt) {
        std::cout << green() << "  " << by << "此步为最优" << reset();
        if (cur->rank == 2) std::cout << "（胜势，最快 " << cur->dist << " 步）";
        else if (cur->rank == 1) std::cout << "（保持和棋）";
        else if (cur->rank == 0) std::cout << "（必败，唯一最长抵抗 " << cur->dist << " 步）";
        std::cout << "\n";
        return;
    }
    const char* mover = by.c_str();
    if (best_before->rank == 2) {
        if (cur->rank == 2) std::cout << gold() << "  ⚠ " << mover << "仍可胜但更慢: 最优 "
                          << best_before->dist << " 步，此步后 " << cur->dist << " 步" << reset() << "\n";
        else if (cur->rank == 1) std::cout << red() << "  ⚠ " << mover << "错失胜机，此步后成和棋" << reset() << "\n";
        else std::cout << red() << "  ⚠ " << mover << "错失胜机，此步后转入必败" << reset() << "\n";
    } else if (best_before->rank == 1) {
        if (cur->rank == 1) std::cout << gold() << "  ⚠ " << mover << "让出胜机: 本可保和，此步后" << cur->v.label() << reset() << "\n";
        else std::cout << red() << "  ⚠ " << mover << "从和棋送成必败" << reset() << "\n";
    } else {
        if (cur->rank == 2) std::cout << green() << "  妙手！" << mover << "逆转取胜" << reset() << "\n";
        else if (cur->rank == 1) std::cout << green() << "  妙手！" << mover << "挽成和棋" << reset() << "\n";
        else std::cout << gold() << "  ⚠ " << mover << "仍必败（最优抵抗 " << best_before->dist
                       << " 步，此步后 " << cur->dist << " 步）" << reset() << "\n";
    }
}

// ---------- 最优路线（双方 DTM 最优） ----------
void print_pv(TbCache& cache, State s, int max_plies) {
    auto terminal_result = [](const State& st) -> Result { return is_terminal(st); };
    int ply = 0;
    while (ply < max_plies) {
        Result tr = terminal_result(s);
        if (tr != Result::UNKNOWN) {
            const char* r = tr == Result::WOLF_WIN ? "狼胜（终局）" :
                            tr == Result::SHEEP_WIN ? "羊胜（终局）" : "平局（150步）";
            std::cout << "  " << std::string(gray()) << r << reset() << "\n";
            return;
        }
        Verdict v = lookup(cache, s);
        if (!v.known) {
            std::cout << "  " << gray() << "（k=" << sheep_count(s)
                      << " 未求解，路线到此为止）" << reset() << "\n";
            return;
        }
        auto evals = evaluate_moves(cache, s);
        if (evals.empty()) { std::cout << "  （无合法走法）\n"; return; }
        const MoveEval* best = &evals[0];
        for (const auto& e : evals) if (move_better(e, *best)) best = &e;

        std::cout << "  " << (s.turn ? std::string(red()) + "狼" + reset()
                                     : std::string(cyan()) + "羊" + reset())
                  << " " << std::setw(3) << ply + 1 << ". "
                  << move_desc(best->m);
        if (best->v.known) std::cout << "   [" << best->v.label() << "]";
        else std::cout << "   [未知]";
        if (best->m.captured >= 0) std::cout << "  (k=" << best->succ_k << ")";
        std::cout << "\n";
        s = apply(s, best->m);
        ++ply;
    }
    std::cout << "  " << gray() << "（达到路线长度上限 " << max_plies << " 步）" << reset() << "\n";
}

// ---------- 交互摆盘 ----------
bool interact_setup(State& s) {
    std::cout << "交互摆盘：输入 5 行，每行 5 个字符（w=狼 s=羊 .=空），"
              << "然后输入回合方（w/s）与已走步数。例:\n"
              << "  www1s/ss1ss/1s3/5/5 或逐行 'ww.s.' '....|' 风格均可\n"
              << gray() << "（更简单：直接输入 FEN 字符串，如 www1s/ss1ss/1s3/5/5 s 0）" << reset() << "\n";
    std::string line;
    std::cout << "> ";
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && (line.find('/') != std::string::npos || line.find(' ') != std::string::npos)) {
        return parse_fen(line, s);
    }
    // 逐行 5x5
    std::string board;
    for (int r = 0; r < 5; ++r) {
        std::cout << "第 " << (r + 1) << " 行> ";
        if (!std::getline(std::cin, line)) return false;
        board += line;
    }
    std::cout << "回合方(w狼/s羊)> ";
    if (!std::getline(std::cin, line)) return false;
    std::string fen = board.substr(0, 5) + "/" + board.substr(5, 5) + "/" +
                      board.substr(10, 5) + "/" + board.substr(15, 5) + "/" +
                      board.substr(20, 5) + " " + (line == "s" ? "s" : "w") + " 0";
    return parse_fen(fen, s);
}

void print_help() {
    std::cout << bold() << "狼羊棋·终端解棋器" << reset() << "（基于逆推表库，只读）\n"
              << "  m a1 b2    走子（当前回合方；复盘时交替输入双方走法）\n"
              << "  ai / best  当前局面全部走法评估 + 最优应手（★）\n"
              << "  play       表库为当前回合方走出最优应手（人机对练）\n"
              << "  pv [n]     双方都最优的完整路线（默认 20 步）\n"
              << "  setup      摆盘评估（FEN 或逐行输入）\n"
              << "  load <i>   载入预设局面；list 列出全部预设\n"
              << "  undo       悔一步   pos 打印 FEN   rot 旋转显示\n"
              << "  new        回到初始局面(k=15)  refresh 重扫已求解桶\n"
              << "  h          帮助   q 退出\n"
              << "坐标 a1..e5；每一步后自动给出点评与下一步参考。\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string data_dir = "data/tb_test";
    int preset_index = -1;
    bool auto_reply = false;

    conui::g_color = isatty(STDOUT_FILENO);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--preset" && i + 1 < argc) preset_index = std::stoi(argv[++i]);
        else if (a == "--auto") auto_reply = true;
        else if (a == "--no-color") conui::g_color = false;
        else { std::cerr << "未知参数: " << a << "\n"; return 1; }
    }

    init_binom();
    init_wolf_info();

    std::cout << "=== 狼羊棋·终端解棋器 ===\n";
    std::cout << "数据目录: " << data_dir << "\n";
    TbCache cache(data_dir);
    auto solved = cache.available();
    std::cout << "已求解桶: k=";
    if (solved.empty()) std::cout << "（无，请先运行 solve_retro）";
    else {
        for (size_t i = 0; i < solved.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << solved[i];
        }
        std::cout << "（" << (solved.back() >= K_MAX ? "全部完成" : "其余桶求解中/未求解") << "）";
    }
    std::cout << "\n\n";

    State s;
    bool flip = false;
    if (preset_index >= 0 && preset_index < static_cast<int>(presets().size())) {
        parse_fen(presets()[preset_index].fen, s);
    } else {
        s = initial_state();  // 初始 k=15（若未求解，表库会提示）
    }
    // 进入 REPL
    std::vector<State> history;  // 悔棋
    print_help();
    print_board(s, flip);
    print_position(s);
    print_verdict_line(lookup(cache, s));

    std::string line;
    auto redraw = [&]() {
        std::cout << "\n";
        print_board(s, flip);
        print_position(s);
        print_verdict_line(lookup(cache, s));
    };
    auto make_move = [&](const Move& m, const std::vector<MoveEval>& before,
                         const std::string& by) {
        // before: 走子前局面的评估（用于点评）
        State after = apply(s, m);
        history.push_back(s);
        s = after;
        redraw();
        if (!before.empty()) critique(cache, s, m, before, by);
        print_moves_eval(cache, s, "  ");
    };

    while (true) {
        std::cout << green() << "解棋> " << reset();
        if (!std::getline(std::cin, line)) break;
        auto toks = tokenize(line);
        if (toks.empty()) continue;
        const std::string& c = toks[0];

        if (c == "q" || c == "quit" || c == "exit") break;
        if (c == "h" || c == "help" || c == "?") { print_help(); continue; }
        if (c == "b") { redraw(); continue; }
        if (c == "rot") { flip = !flip; print_board(s, flip); continue; }
        if (c == "pos" || c == "fen") {
            std::cout << to_fen(s) << "\n"; continue;
        }
        if (c == "refresh") {
            auto sv = cache.available();
            std::cout << "已求解桶: k=";
            for (size_t i = 0; i < sv.size(); ++i) { if (i) std::cout << ","; std::cout << sv[i]; }
            std::cout << "\n";
            continue;
        }
        if (c == "new") { s = initial_state(); redraw(); continue; }
        if (c == "undo") {
            if (history.empty()) { std::cout << gray() << "（没有可悔的棋）" << reset() << "\n"; continue; }
            s = history.back(); history.pop_back();
            redraw(); continue;
        }
        if (c == "list") {
            for (size_t i = 0; i < presets().size(); ++i)
                std::cout << "  [" << i << "] " << presets()[i].name << "  " << presets()[i].fen << "\n";
            continue;
        }
        if (c == "load") {
            int idx = (toks.size() >= 2) ? std::stoi(toks[1]) : -1;
            if (idx < 0 || idx >= static_cast<int>(presets().size())) {
                std::cout << gray() << "（预设下标越界，输入 list 查看）" << reset() << "\n";
                continue;
            }
            State ns; if (parse_fen(presets()[idx].fen, ns)) { s = ns; history.clear(); redraw(); }
            else std::cout << gray() << "（预设 FEN 解析失败）" << reset() << "\n";
            continue;
        }
        if (c == "setup") {
            State ns;
            if (toks.size() >= 2) {
                std::string fen;
                for (size_t i = 1; i < toks.size(); ++i) { if (i > 1) fen += " "; fen += toks[i]; }
                if (!parse_fen(fen, ns)) { std::cout << gray() << "（FEN 解析失败）" << reset() << "\n"; continue; }
            } else {
                if (!interact_setup(ns)) continue;
            }
            if (ns == s) { std::cout << gray() << "（与当前局面相同）" << reset() << "\n"; continue; }
            s = ns; history.clear();
            redraw();
            if (sheep_count(s) <= solved.back() && !solved.empty()) {
                // 摆盘后直接给出评估
                print_moves_eval(cache, s);
            }
            continue;
        }
        if (c == "ai" || c == "best") { print_moves_eval(cache, s); continue; }
        if (c == "pv") {
            int n = (toks.size() >= 2) ? std::max(1, std::stoi(toks[1])) : 20;
            std::cout << "最优路线（双方 DTM 最优，最多 " << n << " 步）:\n";
            print_pv(cache, s, n);
            continue;
        }
        if (c == "play") {
            if (is_terminal(s) != Result::UNKNOWN) {
                std::cout << gray() << "（对局已结束）" << reset() << "\n"; continue;
            }
            auto evals = evaluate_moves(cache, s);
            if (evals.empty()) { std::cout << gray() << "（无合法走法）" << reset() << "\n"; continue; }
            bool any_known = false;
            for (const auto& e : evals) if (e.rank >= 0) { any_known = true; break; }
            if (!any_known) {
                std::cout << gray() << "（表库未求解当前 k=" << sheep_count(s)
                          << "，无法给出最优应手；可先 m 走子或 load 已求解预设）"
                          << reset() << "\n";
                continue;
            }
            const MoveEval* best = &evals[0];
            for (const auto& e : evals) if (move_better(e, *best)) best = &e;
            if (best->rank < 0) { std::cout << gray() << "（所有后继均未求解，无法应手）" << reset() << "\n"; continue; }
            std::cout << "表库应手: " << move_desc(best->m) << "（" << best->v.label() << "）\n";
            make_move(best->m, evals, "表库");
            continue;
        }
        if (c == "m" || c == "move") {
            if (is_terminal(s) != Result::UNKNOWN) {
                std::cout << gray() << "（对局已结束）" << reset() << "\n"; continue;
            }
            if (toks.size() < 3) { std::cout << gray() << "（用法: m a1 b2）" << reset() << "\n"; continue; }
            int r1, c1, r2, c2;
            if (!parse_cell(toks[1], r1, c1) || !parse_cell(toks[2], r2, c2)) {
                std::cout << gray() << "（坐标格式: a1..e5）" << reset() << "\n"; continue;
            }
            Piece pfrom = piece_at(s, pos(r1, c1));
            if (pfrom == Piece::NONE) { std::cout << gray() << "（该格无棋子）" << reset() << "\n"; continue; }
            if ((pfrom == Piece::WOLF) != s.turn) {
                std::cout << gray() << "（不是当前回合方的棋子：" << turn_label(s) << "行动中）" << reset() << "\n";
                continue;
            }
            Move chosen(-1, -1);
            for (const Move& mm : gen_moves_from(s, pos(r1, c1))) {
                if (mm.to == pos(r2, c2)) { chosen = mm; break; }
            }
            if (chosen.from < 0) { std::cout << gray() << "（非法走法）" << reset() << "\n"; continue; }
            auto before = evaluate_moves(cache, s);
            std::string by = s.turn ? "狼方" : "羊方";
            make_move(chosen, before, by + "走");
            std::cout << "\n";
            if (auto_reply) {
                // 自动表库应手
                auto evals2 = evaluate_moves(cache, s);
                const MoveEval* best = nullptr;
                for (const auto& e : evals2) {
                    if (e.rank >= 0 && (!best || move_better(e, *best))) best = &e;
                }
                if (best) {
                    std::cout << "表库自动应手: " << move_desc(best->m)
                              << "（" << best->v.label() << "）\n";
                    before = evals2;
                    make_move(best->m, before, "表库");
                }
            }
            continue;
        }
        std::cout << gray() << "（无法识别的输入，输入 h 查看帮助）" << reset() << "\n";
    }
    std::cout << "再见。\n";
    return 0;
}