#!/usr/bin/env python3
"""自动对局 A/B：用/不用开局库的胜负分布（真实 Web 模型策略）。
用法：python3 tools/bench_opening_book.py --cfg 0..3 --n 80
结果一行输出：配置名 狼胜 羊胜 和棋 平均步数 和棋判因(重复/150)"""
import argparse
import random
import sys
import time

sys.path.insert(0, ".")
from web.server import Tablebase, Session  # noqa: E402
from wolves_eat_sheep_game.rules import WOLF, SHEEP, DRAW  # noqa: E402

CONFIGS = [
    ("双方有书", True, True),
    ("双方无书", False, False),
    ("狼有书·羊无书", True, False),
    ("狼无书·羊有书", False, True),
]
WINNER_LABEL = {WOLF: "狼胜", SHEEP: "羊胜", DRAW: "和棋"}


def play_game(tb, book_wolf, book_sheep):
    sess = Session(tb)
    saved = sess.opening_book, sess.book_max_plies
    g = sess.game
    while g.winner is None:
        want = book_wolf if g.turn == WOLF else book_sheep
        if want:
            sess.opening_book, sess.book_max_plies = saved
        else:
            sess.opening_book, sess.book_max_plies = None, 0
        ch = sess._model_choice()
        if ch is None:
            sleep_back = True
            sess.opening_book, sess.book_max_plies = saved
            return "未决", g.move_count, None
        g.move(ch["from"], ch["to"])
        sess._seen.add(sess._pos_key(g))
        sess._pick_cache = None
    sess.opening_book, sess.book_max_plies = saved
    cause = None
    if g.winner == DRAW:
        cause = "150步" if g.max_moves is not None and g.move_count >= g.max_moves else "5次重复"
    return g.winner, g.move_count, cause


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cfg", type=int, required=True)
    ap.add_argument("--n", type=int, default=80)
    args = ap.parse_args()
    label, bw, bs = CONFIGS[args.cfg]
    tb = Tablebase("data/ws_tb_dtc_260819")
    agg = {WOLF: 0, SHEEP: 0, DRAW: 0, "未决": 0}
    rep = cap = 0
    plies = []
    t0 = time.time()
    for i in range(args.n):
        random.seed(args.cfg * 100000 + i)
        w, n, cause = play_game(tb, bw, bs)
        agg[w if w in agg else "未决"] += 1
        if cause == "5次重复":
            rep += 1
        elif cause == "150步":
            cap += 1
        plies.append(n)
    avg = sum(plies) / len(plies)
    n = args.n
    print(f"{label} | 狼胜 {agg[WOLF]} ({agg[WOLF]/n*100:.1f}%) | "
          f"羊胜 {agg[SHEEP]} ({agg[SHEEP]/n*100:.1f}%) | "
          f"和棋 {agg[DRAW]} ({agg[DRAW]/n*100:.1f}%) | "
          f"平均步数 {avg:.1f} | 和棋判因 重复{rep}/150步{cap} | 耗时{time.time()-t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()