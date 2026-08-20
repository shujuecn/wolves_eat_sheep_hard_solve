#!/usr/bin/env python3
"""web/exporter.py — 最优棋谱导出引擎（纯 Python 标准库，无第三方依赖）

从"当前局面"出发，按硬解表库为**当前走棋方**导出 ≤3 条"赢或和"的完整最优棋谱：

- 导出对象 = 当前走棋方在该局面的表库最优结局（狼胜 / 羊胜 / 和棋）；
  若当前走棋方必败（最佳结局是对方胜），则没有赢/和棋谱可导出，返回明确提示。
- 棋谱**不把对手当会犯错的**：对手每一步都走"最强防守"——表库意义下拖延最久的
  负着（dist 最大），绝不主动送分，把取胜路线逼到最窄、最长；走棋方则必须在表库
  保证内步步精确地推进（同一档内取最快推进的胜着，保证官方规则 150 步内真能走到
  终局——若贪心地原地踱步拖延，会在预算内被官方判和而拿不到"赢"），并回避自身
  来回踱步（防止触发官方同子反复判和），于是整谱全程的总长度就完全由对手的
  **最长拖延防守**决定：**最长、最窄、需步步精确**、最容易被对手反牵制的强制胜线
  （和棋棋谱则由官方判和规则给出确定性和棋终局）。
- 每条棋谱用官方规则实测校验：只有真实走到"获胜方胜"或"和棋"终局的线才会被导出。
- 每一步输出：步号、走子方、起/落点、吃子、走完后的表库结论，以及**走棋前的棋盘
  布局**（供前端子图渲染高亮：起子金环 / 落点绿点 / 吃子红环，与网页版"鼠标悬浮
  最优解"的棋盘预览效果完全一致）。

供 web/server.py 的 Session.export_lines() 调用；也可独立命令行冒烟测试。
"""
import copy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(ROOT / "wolves_eat_sheep_game") not in sys.path:
    sys.path.insert(0, str(ROOT / "wolves_eat_sheep_game"))

from rules import GameState, WOLF, SHEEP, DRAW, SHEEP_BLOCKED, IDLE_LIMIT  # noqa: E402
import hard_solve_fast as hsf  # noqa: E402

# 安全上限（防御性；官方规则本身已含 150 步上限，这里只防无穷循环）
MAX_FIRST_CANDIDATES = 24      # 最多为多少个首着候选生成整谱
WIN_PLY_CAP = 160              # 非无尽胜线步数上限（官方 150 + 余量）
WIN_PLY_CAP_ENDLESS = 400      # 无尽模式胜线上限（官方无上限，防御性截断）
DRAW_PLY_CAP = 200             # 和线步数上限（无尽模式下也强制收尾）

TB_LABEL = {hsf.WOLF_WIN: "狼胜", hsf.SHEEP_WIN: "羊胜", hsf.DRAW: "和棋", hsf.UNKNOWN: "未知"}


def move_label(result: int, dist: int) -> str:
    lab = TB_LABEL.get(result, "未知")
    if result in (hsf.WOLF_WIN, hsf.SHEEP_WIN):
        lab += f"·最快{dist}步"
    return lab


def _pos_key(g: GameState):
    """局面唯一 key（棋盘 + 轮到谁走），防止棋谱在同局面循环。"""
    return (tuple(g.board[i // 5][i % 5] for i in range(25)), g.turn)


def _side_name(side) -> str:
    return "wolf" if side == WOLF else "sheep"


def _win_terminal(champion: str):
    """champion 获胜时 rules.GameState.winner 的值（狼→WOLF；羊→SHEEP_BLOCKED）。"""
    return WOLF if champion == "wolf" else SHEEP_BLOCKED


def _terminal_of(g: GameState):
    """官方规则下 g 的终局归属：'wolf'/'sheep'/'draw'/None。"""
    if g.winner is None:
        return None
    if g.winner == DRAW:
        return "draw"
    if g.winner == WOLF:
        return "wolf"
    if g.winner == SHEEP_BLOCKED:
        return "sheep"
    return None


def _chip(tb, g: GameState) -> str:
    """走完后局面的结论 chip（与对局记录风格的终局/表库标签一致）。"""
    t = _terminal_of(g)
    if t == "wolf":
        return "狼胜·终局"
    if t == "sheep":
        return "羊胜·终局"
    if t == "draw":
        return "和棋·终局"
    known, result, dist = tb.lookup(g)
    if not known:
        return f"表库 k={g.sheep_count}"
    return move_label(result, dist)


def _terminal_text(g: GameState):
    """终局标签与原因。"""
    t = _terminal_of(g)
    if t == "wolf":
        return "狼胜·终局", "羊被吃到不足 4 只"
    if t == "sheep":
        return "羊胜·终局", "三只狼均无法移动"
    if t == "draw":
        if g.max_moves is not None and g.move_count >= g.max_moves:
            return "和棋·终局", f"双方合计 {g.max_moves} 步判和"
        if all(g.idle_streaks[s] >= IDLE_LIMIT for s in (WOLF, SHEEP)):
            return "和棋·终局", "双方同子反复踱步判和（官方规则）"
        return "和棋·终局", "官方规则判和"
    return ("未终局", "")


def _scored_moves(tb, g: GameState, champion: str):
    """当前走棋方全部合法走法的表库评估。

    返回列表，每项：from/to/captured、rank（按走子方利益：2 胜 / 1 和 / 0 负）、
    dist（表库距离，越大越慢）、osc（该子踱步计数，越大越接近判和）、
    key（走完后局面 key）、trial_winner（官方规则下的即时终局，None=未终局）。
    """
    out = []
    my_win = hsf.WOLF_WIN if g.turn == WOLF else hsf.SHEEP_WIN
    for r in range(5):
        for c in range(5):
            if g.board[r][c] != g.turn:
                continue
            for mv in g.legal_moves_from((r, c)):
                trial = copy.deepcopy(g)
                if not trial.move((r, c), mv.destination):
                    continue
                known, result, dist = tb.lookup(trial)
                if not known:
                    continue
                rank = 2 if result == my_win else (1 if result == hsf.DRAW else 0)
                moved_id = trial.piece_ids.get(mv.destination)
                osc = (trial._back_and_forth_count(trial.piece_histories[moved_id])
                       if moved_id in trial.piece_histories else 0)
                out.append({
                    "from": (r, c),
                    "to": mv.destination,
                    "captured": mv.captured,
                    "rank": rank,
                    "dist": dist,
                    "osc": osc,
                    "key": _pos_key(trial),
                    "trial_winner": trial.winner,
                })
    return out


def _pick(moves, taker, champion: str, goal_rank: int, seen) -> dict | None:
    """按棋谱策略从 moves 中选一步。

    - 胜线（goal_rank=2）：
      主角：只走"保持必胜"的着（rank=2），在表库保证内取最快推进（dist 最小，
            保证官方规则 150 步内必达终局——贪心取"最大 dist"会在预算内迟迟吃
            不到子而判和，因此"路程最长"由对手的最强防守承担，主角必须步步精确）；
            同档优先踱步计数小（防官方判和），并回避回到本谱已出现局面。
      对手：只走"仍属主角胜"的着（对它自己 rank=0），选距离最大（拖延最久、最窄
            的求生路），是整谱"最难走/最容易被反牵制"的来源。
    - 和线（goal_rank=1）：双方都只走"不失分"的和着（rank=1），并优先同子反复踱步
      （max osc），让官方判和规则（同子反复踱步 / 150 步上限）给出确定性和棋终局。
    """
    wt = _win_terminal(champion)
    if goal_rank == 1:
        pool = [m for m in moves
                if m["rank"] == 1 and (m["trial_winner"] is None or m["trial_winner"] == DRAW)]
        if not pool:
            return None
        # 双方均按官方判和规则收束：优先同子反复踱步（osc 大）→ 尽快形成"同子反复判和"
        pool.sort(key=lambda m: (-m["osc"], m["from"][0], m["from"][1], m["to"][0], m["to"][1]))
        return pool[0]
    if taker != champion:
        # 对手：最强防守 = 拖延最久（dist 最大）；绝不主动判和/送胜
        pool = [m for m in moves
                if m["rank"] == 0 and (m["trial_winner"] is None or m["trial_winner"] == wt)]
        if not pool:          # 理论不应发生（表库完备性），兜底：任意不破坏胜线的着
            pool = [m for m in moves if m["trial_winner"] is None or m["trial_winner"] == wt]
            if not pool:
                return None
        fresh = [m for m in pool if m["key"] not in seen] or pool
        fresh.sort(key=lambda m: (-m["dist"], m["from"][0], m["from"][1], m["to"][0], m["to"][1]))
        return fresh[0]
    # 主角：保持必胜 + 表库保证内最快推进（dist 最小 → 官方规则内必达终局），
    # 整谱长度由对手最强防守决定；同档回避踱步与循环
    pool = [m for m in moves
            if m["rank"] == 2 and (m["trial_winner"] is None or m["trial_winner"] == wt)]
    if not pool:
        return None
    fresh = [m for m in pool if m["key"] not in seen] or pool
    fresh.sort(key=lambda m: (m["dist"], m["osc"],
                              m["from"][0], m["from"][1], m["to"][0], m["to"][1]))
    return fresh[0]


def _build_line(tb, g0: GameState, champion: str, goal_rank: int,
                first: dict | None, ply_cap: int):
    """沿策略生成整条棋谱（含首着）。返回 (plies, terminal_state) 或 None（线不合法）。"""
    g = copy.deepcopy(g0)
    seen = {_pos_key(g)}
    plies = []
    first_done = first is None
    while g.winner is None:
        if len(plies) >= ply_cap:
            return None
        moves = _scored_moves(tb, g, champion)
        if not moves:
            return None
        chosen = first if not first_done else None
        first_done = True
        if chosen is None:
            chosen = _pick(moves, g.turn, champion, goal_rank, seen)
            if chosen is None:
                return None
        board_before = [g.board[i // 5][i % 5] for i in range(25)]
        side = _side_name(g.turn)
        n = g.move_count + 1
        if not g.move(chosen["from"], chosen["to"]):
            return None
        captured = chosen["captured"] if chosen["captured"] else None
        plies.append({
            "n": n,
            "side": side,
            "from": chosen["from"],
            "to": chosen["to"],
            "captured": captured,
            "board": board_before,
            "label_after": _chip(tb, g),
            "winner_after": g.winner,
        })
        seen.add(_pos_key(g))
    # 官方规则实测校验：胜线必须真胜，和线必须真和
    if goal_rank == 2:
        if _terminal_of(g) != champion:
            return None
    else:
        if _terminal_of(g) != "draw":
            return None
    return plies, g


def _package_line(tb, plies, end: GameState) -> dict:
    moves_out = []
    for p in plies:
        moves_out.append({
            "n": p["n"],
            "side": p["side"],
            "from": p["from"][0] * 5 + p["from"][1],
            "to": p["to"][0] * 5 + p["to"][1],
            "captured": (p["captured"][0] * 5 + p["captured"][1]) if p["captured"] else None,
            "label_after": p["label_after"],
            "board": p["board"],
        })
    term_label, reason = _terminal_text(end)
    return {
        "plies": len(moves_out),
        "moves": moves_out,
        "final": {
            "board": [end.board[i // 5][i % 5] for i in range(25)],
            "label": term_label,
            "reason": reason,
        },
    }


def run_export(tb, game: GameState, count: int = 1, endless: bool = False,
               champion: str | None = None, target: str | None = None,
               max_steps: int | None = None, progress=None) -> dict:
    """导出 ≤10 条「赢或和」最优棋谱（官方规则实测校验后仅返回真实可达终局的线）。

    champion: 先走方视角（'wolf'/'sheep'）。若与当前局面的实际轮到方不同，
              则只读地把分析设为“假设由该方先行”（不改动对局本身）。
    target:   'wolf_win' / 'sheep_win' / 'draw'；缺省 = 该走棋方的表库最优结局
              （狼先走只能选 狼胜/和棋；羊先走只能选 羊胜/和棋）。
              所选目标必须与该视角下表库最优结局一致，否则返回可操作的提示。
    max_steps: 用户设定的最大总步数上限（如 150/160/自定义）；实际总步数超过该值的
              最优解**不进棋谱**。上限只过滤，不写入 PNG（PNG 仅标注实际总步数）。
    progress:  可选回调 progress(pct: float, phase: str)，用于前端进度条/耗时显示。

    返回：
      {"ok": True, "goal":..., "position": {...}, "lines": [ {no,plies,moves,final} ... ]}
      或 {"ok": False, "error": "..."}
    """
    g = copy.deepcopy(game)
    if champion is None:
        champion = "wolf" if g.turn == WOLF else "sheep"
    else:
        champion = "wolf" if champion == "wolf" else "sheep"
        if (g.turn == WOLF) != (champion == "wolf"):
            g.turn = WOLF if champion == "wolf" else SHEEP   # 假设所选方先行（只读视角）
    if progress:
        progress(1.0, "初始化…")
    my_win = hsf.WOLF_WIN if champion == "wolf" else hsf.SHEEP_WIN
    cn = "狼" if champion == "wolf" else "羊"
    known, result, dist = tb.lookup(g)
    if not known:
        return {"ok": False, "error": f"表库 k={g.sheep_count} 未求解，无法导出棋谱。"}

    if target in (None, "", "auto"):
        # 缺省：按所选先走方的表库最优结局
        if result == my_win:
            goal_rank, goal_label = 2, "狼胜" if champion == "wolf" else "羊胜"
        elif result == hsf.DRAW:
            goal_rank, goal_label = 1, "和棋"
        else:
            opp = "羊" if champion == "wolf" else "狼"
            return {"ok": False, "error":
                    (f"以 {cn} 先走看当前局面，表库结论为 {opp} 方必胜（{move_label(result, dist)}），"
                     f"当前视角已必败，无法生成最优棋谱。")}
    elif target == "draw":
        if result != hsf.DRAW:
            if result == my_win:
                return {"ok": False, "error":
                        f"以 {cn} 先走为视角，表库结论是 {move_label(result, dist)}，当前视角可胜，"
                        f"目标「和棋」非最优，请改选「{'狼胜' if champion == 'wolf' else '羊胜'}」。"}
            return {"ok": False, "error":
                    f"以 {cn} 先走为视角，表库结论是 {move_label(result, dist)}，当前视角已必败，"
                    f"目标「和棋」无法生成棋谱。"}
        goal_rank, goal_label = 1, "和棋"
    else:
        if target not in ("wolf_win", "sheep_win") or (target == "wolf_win") != (champion == "wolf"):
            return {"ok": False, "error": "目标与先走方不一致：狼先走只能选「狼胜/和棋」，羊先走只能选「羊胜/和棋」。"}
        if result != my_win:
            hint = "和棋" if result == hsf.DRAW else "无（当前视角已必败）"
            return {"ok": False, "error":
                    f"以 {cn} 先走为视角，表库结论是 {move_label(result, dist)}，"
                    f"目标「{'狼胜' if target == 'wolf_win' else '羊胜'}」不可达成；可改选「{hint}」。"}
        goal_rank, goal_label = 2, "狼胜" if target == "wolf_win" else "羊胜"

    count = max(1, min(int(count or 1), 10))   # 1～10 条（受制于候选首着多样性与 150/无尽步预算，可能实际更少）

    firsts = _scored_moves(tb, g, champion)
    if progress:
        progress(3.0, "分析当前局面最优着法…")
    wt = _win_terminal(champion)
    if goal_rank == 2:
        cands = [m for m in firsts
                 if m["rank"] == 2 and (m["trial_winner"] is None or m["trial_winner"] == wt)]
        cands.sort(key=lambda m: (-m["dist"], m["osc"],
                                  m["from"][0], m["from"][1], m["to"][0], m["to"][1]))
    else:
        cands = [m for m in firsts
                 if m["rank"] == 1 and (m["trial_winner"] is None or m["trial_winner"] == DRAW)]
        cands.sort(key=lambda m: (m["from"][0], m["from"][1], m["to"][0], m["to"][1]))
    if not cands:
        return {"ok": False, "error": "当前局面的最优档首着为空，无法导出棋谱。"}

    if goal_rank == 2:
        ply_cap = WIN_PLY_CAP_ENDLESS if endless else WIN_PLY_CAP
    else:
        ply_cap = DRAW_PLY_CAP

    built = []
    pool = cands[:MAX_FIRST_CANDIDATES]
    for i, fm in enumerate(pool):
        if progress:
            progress(5.0 + 88.0 * i / max(len(pool), 1), f"推算候选线 {i + 1}/{len(pool)}…")
        line = _build_line(tb, g, champion, goal_rank, first=fm, ply_cap=ply_cap)
        if line is None:
            continue
        plies, end = line
        built.append(_package_line(tb, plies, end))
    # 用户设定最大总步数：超过该值的最优解不进棋谱（只过滤，不写入 PNG）
    if max_steps:
        try:
            max_steps = int(max_steps)
        except (TypeError, ValueError):
            max_steps = None
        if max_steps and max_steps > 0:
            kept = [ln for ln in built if ln["plies"] <= max_steps]
            if not kept:
                return {"ok": False, "error":
                        f"所有最优解实际总步数均超过设定上限 {max_steps} 步，请调大最大步数后再试"
                        f"（可先开启无尽模式）。"}
            built = kept
    # 按真实全程步数降序取前 count 条（路程最长者优先）
    if progress:
        progress(97.0, "按最长步数排序并做最终校验…")
    built.sort(key=lambda L: L["plies"], reverse=True)
    lines = built[:count]
    for i, ln in enumerate(lines, 1):
        ln["no"] = i
    if progress:
        progress(100.0, "完成")

    position = {
        "fen": _fen(g),
        "turn": "wolf" if g.turn == WOLF else "sheep",
        "sheep": g.sheep_count,
        "move_count": g.move_count,
        "verdict": move_label(result, dist),
    }
    return {"ok": True,
            "goal": goal_label,
            "goal_rank": goal_rank,
            "position": position,
            "requested": count,
            "lines": lines}


def _fen(g: GameState) -> str:
    rows = []
    for r in range(5):
        row, empty = "", 0
        for c in range(5):
            p = g.board[r][c]
            if p is None:
                empty += 1
            else:
                if empty:
                    row += str(empty)
                    empty = 0
                row += "w" if p == WOLF else "s"
        if empty:
            row += str(empty)
        rows.append(row)
    return "/".join(rows) + " " + ("w" if g.turn == WOLF else "s") + " " + str(g.move_count)


# ============================================================
# 命令行冒烟测试
# ============================================================
def main():
    import argparse
    import time
    ap = argparse.ArgumentParser(description="最优棋谱导出引擎 · 冒烟测试")
    ap.add_argument("--data-dir", default=str(ROOT / "data" / "ws_tb_dtc_260819"))
    ap.add_argument("--fen", default=None, help="局面 FEN（缺省用内置样例）")
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--endless", action="store_true")
    ap.add_argument("--champion", default=None, choices=["wolf", "sheep"], help="先走方视角")
    ap.add_argument("--target", default=None, choices=["wolf_win", "sheep_win", "draw"], help="目标结局")
    args = ap.parse_args()

    from web.server import Tablebase, game_from_fen  # noqa: PLC0415  (复用 FEN 解析)

    tb = Tablebase(args.data_dir)
    samples = {
        "狼胜·狼先": "s2ws/ss3/5/3w1/2w2 w 36",
        "和棋·羊先": "sssss/sssss/sswss/5/1w1w1 s 1",
        "羊胜·羊先": "sssss/sssss/1swss/1s2w/1w3 s 4",
        "必败·狼先(羊胜)": "sssss/sssss/1swss/1s2w/1w3 w 4",
    }
    fens = [args.fen] if args.fen else list(samples.values())
    for fen in fens:
        print("=" * 78)
        print("局面:", fen)
        g = game_from_fen(fen, max_moves=None if args.endless else 150, idle_limit=IDLE_LIMIT)
        t0 = time.time()
        res = run_export(tb, g, count=args.count, endless=args.endless,
                         champion=args.champion, target=args.target)
        dt = time.time() - t0
        print(f"耗时 {dt:.2f}s")
        if not res["ok"]:
            print("  ✗", res["error"])
            continue
        print(f"  目标: {res['goal']} · 请求 {res['requested']} 条 · 实际 {len(res['lines'])} 条")
        for ln in res["lines"]:
            mv0 = ln["moves"][0]
            mv1 = ln["moves"][1]
            dest = ln["final"]["board"]
            print(f"  解法{ln['no']}: 步数={ln['plies']}  首着=({mv0['from']}→{mv0['to']},吃{mv0['captured']})"
                  f" 次着=({mv1['from']}→{mv1['to']}) 终局={ln['final']['label']}({ln['final']['reason']})")
            # 校验：每步棋盘为 25 元素；终局合法
            for m in ln["moves"]:
                assert len(m["board"]) == 25, "board 长度错误"
            assert len(dest) == 25, "final board 长度错误"
        # 校验首条可完整回放
        if res["ok"] and res["lines"]:
            _replay_check(g, res["lines"][0])


def _replay_check(g0: GameState, line) -> None:
    g = copy.deepcopy(g0)
    for i, m in enumerate(line["moves"]):
        cur = [g.board[i0 // 5][i0 % 5] for i0 in range(25)]
        assert cur == m["board"], f"步{i+1} 走棋前棋盘与记录不符"
        assert g.turn == (WOLF if m["side"] == "wolf" else SHEEP), f"步{i+1} 走子方不符"
        assert g.move_count + 1 == m["n"], f"步{i+1} 步号不符"
        fr, to = (m["from"] // 5, m["from"] % 5), (m["to"] // 5, m["to"] % 5)
        sheep_before = g.sheep_count
        ok = g.move(fr, to)
        assert ok, f"步{i+1} 非法走法"
        if m["captured"] is not None:
            assert g.sheep_count == sheep_before - 1, f"步{i+1} 吃子校验失败"
    print(f"  ✓ 回放校验通过：{len(line['moves'])} 步，终局 winner={g.winner}")


if __name__ == "__main__":
    main()