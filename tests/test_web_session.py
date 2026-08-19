#!/usr/bin/env python3
"""集成验证：悔棋记录修复 / 终局+重开自动存档 / 无尽模式（>150 步续走+提示+记录）。

注意：存档目录被重定向到 tests/.saved_games_tmp，不会动真实 data/saved_games。
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "wolves_eat_sheep_game"))

from web import server as srv  # noqa: E402

tb = srv.Tablebase(str(ROOT / "data" / "ws_tb_dtc_260819"))
# 存档目录指向临时目录（save_record/saved_count 读取的是模块全局 SAVED_DIR）
_TMP = Path(tempfile.mkdtemp(prefix="ws_saved_games_"))
srv.SAVED_DIR = _TMP
SAVED = srv.SAVED_DIR
ok_count = 0


def check(name, cond, extra=""):
    global ok_count
    assert cond, f"FAIL: {name} {extra}"
    ok_count += 1
    print(f"  ok  {name}" + (f"  {extra}" if extra else ""))


def first_legal_pick(game):
    for r in range(5):
        for c in range(5):
            if game.board[r][c] != game.turn:
                continue
            for mv in game.legal_moves_from((r, c)):
                return r, c, mv.destination[0], mv.destination[1]
    return None


def session_play(ses, plies):
    """当前回合方交替走 plies 步：轮到玩家就 move，轮到模型就 advance。"""
    for _ in range(plies):
        if ses.game.turn == ses.player_side:
            pick = first_legal_pick(ses.game)
            ok, err = ses.move(pick[0] * 5 + pick[1], pick[2] * 5 + pick[3])
            assert ok, err
        else:
            ses.advance()


# 清空测试存档，保证计数确定
if SAVED.is_dir():
    for p in SAVED.glob("game_*.json"):
        p.unlink()

# ============ 1. 悔棋：记录只按步数截断，玩家/模型记录都保留 ============
print("[1] undo 记录修复")
s1 = srv.Session(tb)
s1.choose_side("wolf")
session_play(s1, 6)                       # 3 玩家 + 3 模型，共 6 步
check("history 每步一条 (7 = 初始+6)", len(s1.history) == 7, f"len={len(s1.history)}")
ok, err = s1.undo()
assert ok
check("悔1步: log 剩 5 条", len(s1.log) == 5, f"len={len(s1.log)}")
for i, e in enumerate(s1.log):
    check(f"log[{i}] n/fen 与 history 对齐", e["n"] == i + 1 and e["fen"] == s1.history[i + 1])
sides = [e["side"] for e in s1.log]
check("玩家与模型记录都在", "wolf" in sides and "sheep" in sides, str(sides))
check("悔棋只回退 1 步", s1.game.move_count == 5, f"mc={s1.game.move_count}")

# 悔完后轮到模型(羊)：让它走一步，玩家再走一步 → 7 步；再悔一次 → 回退到 6
s1.advance()
session_play(s1, 1)
check("走到 7 步", s1.game.move_count == 7 and len(s1.log) == 7)
ok, _ = s1.undo()
check("模型走后悔棋: 回退 1 步到 6", s1.game.move_count == 6 and len(s1.log) == 6,
      f"mc={s1.game.move_count}")

while s1.undo()[0]:
    pass
check("悔到起点: 记录清空", s1.log == [] and len(s1.history) == 1)

# ============ 2. 终局自动存档 ============
print("[2] 终局自动存档")
s2 = srv.Session(tb)
s2.choose_side("wolf")
s2.game.move_count = 149                  # 逼到 150 步判和
ok, err = s2.move(4 * 5 + 1, 3 * 5 + 1)   # 狼上移一步 → 第 150 步
assert ok, err
snap = s2.snapshot()
check("终局判定和棋", snap["terminal"] and snap["terminal"]["result"] == "和棋")
check("终局后已自动存档", snap["record_saved"] is True and snap["last_saved"], snap["last_saved"])
check("saved_count >= 1", snap["saved_count"] >= 1)
rec_path = SAVED / snap["last_saved"]
check("存档文件存在", rec_path.exists())
rec = json.loads(rec_path.read_text(encoding="utf-8"))
check("存档内容: 结果/步数", rec["result"] == "和棋" and rec["move_count"] == 150
      and rec["endless"] is False and rec["final_fen"].endswith(" 150"))
check("存档含完整对局记录", isinstance(rec["moves"], list) and len(rec["moves"]) == 1
      and rec["moves"][0]["to"] == 3 * 5 + 1)

before = s2.saved_count()
s2.restart()                              # 已存过 → 不重复存
check("重开不重复存档", s2.saved_count() == before)

# 重开后再走几步、未终局手动重开 → 自动保存“未完成”局
session_play(s2, 4)
check("新一局走 4 步", s2.game.move_count == 4 and len(s2.log) == 4)
s2.restart()
check("未完成局也存档", s2.saved_count() == before + 1)
latest = max(SAVED.glob("game_*.json"), key=lambda p: p.stat().st_mtime)
rec2 = json.loads(latest.read_text(encoding="utf-8"))
check("未完成局结果标注", rec2["result"] == "未完成" and rec2["reason"] == "手动重开"
      and rec2["move_count"] == 4)

# ============ 3. 无尽模式：>150 步继续 + 提示 + 记录 ============
print("[3] 无尽模式")
s3 = srv.Session(tb)
s3.choose_side("wolf")
ok, err = s3.set_endless(True)
assert ok, err
check("无尽模式 max_moves=None", s3.game.max_moves is None)
s3.game.move_count = 148
session_play(s3, 2)                       # 玩家 149 → 模型 150（不限步，无终局）
check("150 步无终局", s3.game.winner is None and s3.game.move_count == 150,
      f"winner={s3.game.winner}")
snap = s3.snapshot()
check("无尽模式快照标记", snap["endless"] is True and snap["terminal"] is None)
check("150 步仍给最优解结论", snap["verdict"]["known"] and snap["verdict"]["label"],
      snap["verdict"]["label"])
check("150 步仍给候选续着", len(snap["analysis"]) > 0, f"analysis={len(snap['analysis'])}")
pick = first_legal_pick(s3.game)
ok, _ = s3.move(pick[0] * 5 + pick[1], pick[2] * 5 + pick[3])   # 玩家 → 151
check("151 步继续对局", s3.game.move_count == 151 and s3.game.winner is None)
snap = s3.snapshot()
check("151 步后记录累积 3 条", len(snap["log"]) == 3 and snap["log"][-1]["n"] == 151)
check("151 步后提示仍有效", snap["verdict"]["known"] and len(snap["analysis"]) > 0)

# 关闭无尽模式 → 立即按 150 步上限判和（同一步数逻辑）
ok, _ = s3.set_endless(False)
check(">150 关无尽 → 立即判和", s3.game.winner == srv.DRAW and s3.game.max_moves == 150)
snap = s3.snapshot()
check("关闭后终局存档一次", snap["record_saved"] is True and snap["terminal"]["result"] == "和棋")

# 已判和后开启无尽 → 解除终局继续下
s3.set_endless(True)
check("判和后开无尽 → 解除终局", s3.game.winner is None and s3.endless)
s3.advance()                              # 模型(羊)先应手 → 152
check("解除后继续: 152 步", s3.game.move_count == 152 and s3.game.winner is None,
      f"mc={s3.game.move_count}")
pick = first_legal_pick(s3.game)
ok, _ = s3.move(pick[0] * 5 + pick[1], pick[2] * 5 + pick[3])   # 玩家 → 153
check("玩家 153 步继续", s3.game.move_count == 153 and s3.game.winner is None)

# 无尽模式下悔棋保留 max_moves=None 且只回退 1 步
session_play(s3, 1)                       # 模型 → 154
log_before = len(s3.log)
ok, _ = s3.undo()
check("无尽模式悔棋保留上限且回退 1 步",
      s3.game.max_moves is None and len(s3.log) == log_before - 1 and s3.game.move_count == 153,
      f"mc={s3.game.move_count} log={len(s3.log)}")

# ============ 4. 存档目录可写且结构完整 ============
print("[4] 存档目录")
check("存档目录存在", SAVED.is_dir())
files = sorted(SAVED.glob("game_*.json"))
check("存档文件可列出", len(files) >= 3, f"count={len(files)}")

print(f"\nALL {ok_count} CHECKS PASSED")
shutil.rmtree(_TMP, ignore_errors=True)   # 测试结束清理临时存档