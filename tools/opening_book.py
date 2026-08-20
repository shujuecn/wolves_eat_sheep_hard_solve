#!/usr/bin/env python3
"""开局库生成与开局统计（纯标准库 + 本仓库表库）。

从初始局面逐层展开开局树（前 BOOK_PLY 个半回合入书，前 STATS_PLY 个半回合统计），
每一步都直查 DTC 表库得到该后继局面的结论（狼胜/和/羊胜）与最快步数。

用户视角的分类：
  - 狼方走子：狼胜 / 和棋 / 羊胜（狼输）；"狼必赢或和" = 狼胜 + 和棋。
  - 羊方走子：羊胜（羊必赢）/ 和棋（按约定算羊输）/ 狼胜（羊输）。

输出：
  - web/opening_book.json   开局库（前 BOOK_PLY 个半回合，按优选序排好的走法表）
  - web/opening_stats.txt   开局统计报告（各深度结局分布、各层走法分类、首次走法明细）

用法：
  python3 tools/opening_book.py [--data-dir data/ws_tb_dtc_260819]
                                [--book-plies 6] [--stats-plies 8]
"""
import argparse
import copy
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from web.server import Tablebase  # noqa: E402  (借其 mmap 打开与桶校验)
import hard_solve_fast as hsf     # noqa: E402  (局面编码/索引用)
from wolves_eat_sheep_game.rules import WOLF, SHEEP  # noqa: E402

RES_LABEL = {0: "狼胜", 1: "羊胜", 2: "和棋", 3: "未知"}


def open_key(cells, turn):
    """局面 key：25 格按行拼串 + 回合。与 web/server.py 的 book key 完全一致。"""
    return "".join(("w" if c == WOLF else "s" if c == SHEEP else ".") for c in cells) + ":" + ("w" if turn == WOLF else "s")


def initial_cells():
    cells = [None] * 25
    for r in range(3):
        for c in range(5):
            cells[r * 5 + c] = SHEEP
    for c in (1, 2, 3):
        cells[4 * 5 + c] = WOLF
    return cells


def gen_moves(cells, turn):
    """返回 [(from_1d, to_1d, captured_1d or None)]，规则同 rules.legal_moves_from。"""
    out = []
    for r in range(5):
        for c in range(5):
            if cells[r * 5 + c] != turn:
                continue
            for dr, dc in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                nr, nc = r + dr, c + dc
                if not (0 <= nr < 5 and 0 <= nc < 5):
                    continue
                if cells[nr * 5 + nc] is not None:
                    continue
                out.append((r * 5 + c, nr * 5 + nc, None))
                pr, pc = r + 2 * dr, c + 2 * dc
                if (turn == WOLF and 0 <= pr < 5 and 0 <= pc < 5
                        and cells[pr * 5 + pc] == SHEEP):
                    out.append((r * 5 + c, pr * 5 + pc, pr * 5 + pc))
    return out


def apply_move(cells, mv):
    fr, to, cap = mv
    nxt = copy.copy(cells)
    nxt[to] = nxt[fr]      # 落点：直走=空格；跳吃=羊格（落点即被吃格，直接覆盖）
    nxt[fr] = None
    if cap is not None and cap != to:
        nxt[cap] = None    # 跳吃时 cap==to，羊已被落子覆盖，无需再删
    return nxt


def sheeps(cells):
    return sum(1 for c in cells if c == SHEEP)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default=os.path.join(ROOT, "data", "ws_tb_dtc_260819"))
    ap.add_argument("--book-plies", type=int, default=6, help="开局库覆盖的半回合数（默认 6）")
    ap.add_argument("--stats-plies", type=int, default=8, help="统计覆盖的半回合数（默认 8）")
    args = ap.parse_args()

    t0 = time.time()
    tb = Tablebase(args.data_dir)
    print(f"[opening_book] 表库 max_k={tb.max_k}，耗时 {time.time()-t0:.1f}s", flush=True)

    def lookup(cells, turn):
        k = sheeps(cells)
        if k < 4:
            return True, hsf.WOLF_WIN, 0
        if not tb._open(k):
            return False, hsf.UNKNOWN, 0
        wr = hsf.encode_wolf([i for i, c in enumerate(cells) if c == WOLF])
        sr = hsf.encode_sheep([i for i, c in enumerate(cells) if c == SHEEP], wr, k)
        idx = hsf.state_index(wr, sr, k, turn == SHEEP)
        e = tb._mm[k][64 + idx]
        return True, e & 3, (e >> 2) & 0x3F

    start = initial_cells()
    level = {open_key(start, WOLF): (start, WOLF)}   # key -> (cells, turn)
    book = {}          # key -> {"r": result, "m": [[fr,to,res,dist], ...] 按优选排序}
    stat_pos = {p: [0, 0, 0] for p in range(args.stats_plies + 1)}   # ply -> [狼胜,羊胜,和]
    stat_mov = {p: [0, 0, 0] for p in range(args.stats_plies + 1)}   # 该层走法(出发)按后果分类
    first_moves = []   # 初始局面每个狼走法 -> (fr,to,结果,步数)

    for ply in range(0, args.stats_plies + 1):
        nxt = {}
        for key, (cells, turn) in level.items():
            known, res, dist = lookup(cells, turn)
            if not known:
                print(f"[opening_book] 警告：k={sheeps(cells)} 未求解的局面 {key}", flush=True)
                continue
            stat_pos[ply][res] += 1
            if ply <= args.book_plies:
                book[key] = {"r": res, "m": []}
            moves = gen_moves(cells, turn)
            my_win = 0 if turn == WOLF else 1
            entries = []
            for fr, to, cap in moves:
                child = apply_move(cells, (fr, to, cap))
                ck, cres, cdist = lookup(child, SHEEP if turn == WOLF else WOLF)
                stat_mov[ply][cres] += 1
                if ply == 0:
                    first_moves.append((fr, to, cres, cdist))
                if ply <= args.book_plies:
                    entries.append([fr, to, cres, cdist])
                if ply < args.stats_plies:
                    nkey = open_key(child, SHEEP if turn == WOLF else WOLF)
                    if nkey not in nxt:
                        nxt[nkey] = (child, SHEEP if turn == WOLF else WOLF)
            if ply <= args.book_plies:
                # 优选序：mover 必胜(最快) -> 和棋 -> 必败(拖最久)；同档按 (fr,to) 定序
                rank = lambda e: (2 if e[2] == my_win else (1 if e[2] == 2 else 0))  # noqa: E731
                entries.sort(key=lambda e: (
                    -rank(e),
                    e[3] if rank(e) == 2 else (0 if rank(e) == 1 else -e[3]),
                    e[0], e[1]))
                book[key]["m"] = entries
        level = nxt
        print(f"[opening_book] ply {ply}: 局面 {stat_pos[ply][0]+stat_pos[ply][1]+stat_pos[ply][2]:,} 个"
              f" | 该层走法 {sum(stat_mov[ply]):,} 条", flush=True)

    # ---- 输出书 ----
    bk = {"version": 1, "max_plies": args.book_plies,
          "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
          "positions": book}
    bpath = os.path.join(ROOT, "web", "opening_book.json")
    with open(bpath, "w", encoding="utf-8") as f:
        json.dump(bk, f, ensure_ascii=False, separators=(",", ":"))
    print(f"[opening_book] 开局库写入 {bpath}：{len(book):,} 个局面，"
          f"{os.path.getsize(bpath)/1024:.0f} KB", flush=True)

    # ---- 统计报告 ----
    lines = []
    lines.append("## 开局统计（前 %d 个半回合）" % args.stats_plies)
    lines.append("")
    lines.append("| ply | 走子方 | 局面数 | 狼胜 | 羊胜 | 和棋 |")
    lines.append("|---:|---:|---:|---:|---:|---:|")
    for p in range(args.stats_plies + 1):
        s = stat_pos[p]
        n = sum(s)
        lines.append(f"| {p} | {'狼' if p % 2 == 0 else '羊'} | {n:,} | "
                     f"{s[0]:,}（{100*s[0]/n:.2f}%） | {s[1]:,}（{100*s[1]/n:.2f}%） | "
                     f"{s[2]:,}（{100*s[2]/n:.2f}%） |")
    lines.append("")

    # 走法层分类（走子方视角：必赢 / 和 / 必败；和棋按"羊方算输"记羊侧）
    lines.append(f"各层走法按后果分类（数量 = 该层出发的全部合法步数）：")
    lines.append("")
    lines.append("| ply | 走子方 | 走法总数 | 走到狼胜 | 走到羊胜 | 走到和棋 | 走子方'能赢或和' |")
    lines.append("|---:|---:|---:|---:|---:|---:|---:|")
    for p in range(args.stats_plies + 1):
        s = stat_mov[p]
        n = sum(s)
        mover = "狼" if p % 2 == 0 else "羊"
        # 能赢或和：狼→狼胜+和；羊→只要羊胜（和算羊输）
        good = (s[0] + s[2]) if mover == "狼" else s[1]
        lines.append(f"| {p} | {mover} | {n:,} | {s[0]:,} | {s[1]:,} | {s[2]:,} | "
                     f"{good:,}（{100*good/n:.2f}%） |")
    lines.append("")

    # 初始局面首次走法明细
    lines.append("初始局面（狼先行）的全部首着（fr→to 为一维格号 0..24，羊数=15）：")
    lines.append("")
    lines.append("| 首着 fr→to | 后继结论 | 最快步数 | 对狼意义 |")
    lines.append("|---|---:|---:|---|")
    for fr, to, cres, cdist in sorted(first_moves, key=lambda x: (x[2], x[3], x[0], x[1])):
        label = RES_LABEL.get(cres, "未知")
        meaning = {"狼胜": "狼可胜", "和棋": "狼保和（羊失和=羊输）", "羊胜": "狼败（羊必赢）"}.get(label, "")
        lines.append(f"| {fr}→{to} | {label} | {cdist} | {meaning} |")
    lines.append("")
    lines.append(f"总用时 {time.time()-t0:.1f}s")
    stat_path = os.path.join(ROOT, "web", "opening_stats.txt")
    with open(stat_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("\n".join(lines), flush=True)
    print(f"[opening_book] 统计报告写入 {stat_path}", flush=True)


if __name__ == "__main__":
    main()