#!/usr/bin/env python3
"""web/server.py — 狼羊棋 · 网页版后端（纯 Python 标准库，无第三方依赖）

- POST /api：choose / move / advance / undo / switch / restart / endless / state
- 自动存档：对局结束或手动重开时，自动把对局记录落盘到 data/saved_games/game_*.json
  （JSON 含每步走子、吃子与走完后的表库最优解结论，便于回放分析）。
- 无尽模式（endless）：勾选后解除 150 步上限，超过后仍继续对局，
  最优解提示与对局记录不受影响（表库结论只依赖局面，与步数无关）。
- 交互节奏：玩家走子后服务端**不立即让模型应手**（响应里 model_to_move=true），
  网页端等待 1 秒后再调 advance，模型才走子（符合“玩家行棋后隔一秒再由模型走棋”）。
- 每步响应包含：
    analysis   —— 当前回合方的多条候选最优续着（表库结论，Top-N）
    log        —— 对局记录：每一步(玩家/模型)的走子 + 走完后局面的最优解结论
    legal      —— 当前回合方全部合法走法（网页点选高亮用）
    verdict    —— 当前局面最优解（狼胜/羊胜/和棋 + 最快步数）
- 引擎逻辑：全部走子决策都基于硬解表库（纯规则逆推，k=4..15 已全部解出，
  开局羊数 15 ≤ 15，整局都在表库覆盖范围内）。不再需要 DQN/神经网络。
  - 羊数 ≤ 已解出最大 k → 表库（mmap 只读）最优解应手
- k 上限不写死：程序启动时快速扫描表库目录（只读各文件 64 字节头，立即完成），
  取"已截完"（magic=WSTB 且 completed 标志=1、尺寸正确）的最大 k 作为分界；
  硬解表库持续扩充后，重启即自动用上新截完的层。

运行（SSH 场景）：
  远程: python3 web/server.py --port 8080
  本机: ssh -L 8080:127.0.0.1:8080 <user>@<host> → 浏览器 http://127.0.0.1:8080
"""
import argparse
import copy
import json
import mmap
import os
import re
import sys
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))                              # hard_solve_fast
sys.path.insert(0, str(ROOT / "wolves_eat_sheep_game"))    # rules

from rules import GameState, WOLF, SHEEP, DRAW  # noqa: E402
import hard_solve_fast as hsf  # noqa: E402

INDEX_PATH = Path(__file__).parent / "index.html"
SAVED_DIR = ROOT / "data" / "saved_games"   # 自动保存的对局记录（JSON 文件）

TB_RESULT_LABEL = {hsf.WOLF_WIN: "狼胜", hsf.SHEEP_WIN: "羊胜", hsf.DRAW: "和棋", hsf.UNKNOWN: "未知"}


def move_label(result: int, dist: int) -> str:
    """结论标签：狼胜·最快9步 / 羊胜·最快12步 / 和棋"""
    lab = TB_RESULT_LABEL.get(result, "未知")
    if result in (hsf.WOLF_WIN, hsf.SHEEP_WIN):
        lab += f"·最快{dist}步"
    return lab


# ============================================================
# 硬解表库（mmap 只读；文件未完成/不存在 → 该 k 不可用）
# ============================================================
TB_FILE_RE = re.compile(r"dtc_k(\d+)\.bin\Z")


def tb_file_completed(path: str, k: int) -> bool:
    """快速校验单个表库文件是否已截完（只读 64 字节头 + stat，不 mmap，立即返回）。

    与 _open 的判定一致：大小 = 64 + bucket_size(k)，magic = WSTB，hdr[24] = 1（completed）。
    """
    try:
        if os.path.getsize(path) != 64 + hsf.bucket_size(k):
            return False
        with open(path, "rb") as f:
            hdr = f.read(64)
        return len(hdr) == 64 and hdr[:4] == b"WSTB" and hdr[24] == 1
    except OSError:
        return False


def max_completed_k(data_dir: str) -> int:
    """启动时扫描表库目录，返回已截完的最大 k（作为表库覆盖的分界）。

    兜底 3：k<4 由对局规则硬编码为狼胜（lookup 直接返回 WOLF_WIN），无需表库。
    """
    best = 3
    try:
        names = os.listdir(data_dir)
    except OSError:
        return best
    for name in names:
        m = TB_FILE_RE.match(name)
        if not m:
            continue
        k = int(m.group(1))
        if k > best and tb_file_completed(os.path.join(data_dir, name), k):
            best = k
    return best


class Tablebase:
    def __init__(self, data_dir: str):
        self.dir = data_dir
        self._mm = {}
        # 程序启动前快速校验：已截完的最大 k，之后固定为本进程的分界 n。
        self.max_k = max_completed_k(data_dir)

    def _open(self, k: int) -> bool:
        if k in self._mm:
            return True
        path = os.path.join(self.dir, f"dtc_k{k:02d}.bin")
        try:
            with open(path, "rb") as f:
                size = os.fstat(f.fileno()).st_size
                if size != 64 + hsf.bucket_size(k):
                    return False
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except OSError:
            return False
        if mm[:4] != b"WSTB" or mm[24] != 1:  # magic + completed
            mm.close()
            return False
        self._mm[k] = mm
        return True

    def lookup(self, game: GameState):
        """返回 (known, result, dist)；result 用 hsf 常量（0狼胜/1羊胜/2和）。"""
        k = game.sheep_count
        if k < 4:
            return True, hsf.WOLF_WIN, 0
        if not self._open(k):
            return False, hsf.UNKNOWN, 0
        gr = hsf.encode_wolf([i for i in range(25) if game.board[i // 5][i % 5] == WOLF])
        sr = hsf.encode_sheep([i for i in range(25) if game.board[i // 5][i % 5] == SHEEP], gr, k)
        idx = hsf.state_index(gr, sr, k, game.turn == SHEEP)
        entry = self._mm[k][64 + idx]
        return True, entry & 3, (entry >> 2) & 0x3F

    def best_move(self, game: GameState):
        """当前回合方的最优走法，返回 ((r1,c1), Move) 或 None（未知/无走法）。"""
        my_win = hsf.WOLF_WIN if game.turn == WOLF else hsf.SHEEP_WIN
        best = None  # (rank, dist, move, key)
        for r in range(5):
            for c in range(5):
                if game.board[r][c] != game.turn:
                    continue
                for mv in game.legal_moves_from((r, c)):
                    trial = copy.deepcopy(game)
                    if not trial.move((r, c), mv.destination):
                        continue
                    known, result, dist = self.lookup(trial)
                    if not known:
                        continue
                    rank = 2 if result == my_win else (1 if result == hsf.DRAW else 0)
                    key = (rank, -dist if rank == 2 else (dist if rank == 0 else 0))
                    if best is None or key > best[3]:
                        best = (rank, dist, ((r, c), mv), key)
        return best[2] if best else None


# ============================================================
# FEN 解析（悔棋恢复局面用；原 ai_engine.game_from_fen 已随 DQN 一并移除以
# 去除 torch/ai_engine 依赖，表库引擎不需要神经网络）
# ============================================================
def game_from_fen(fen: str, max_moves: int | None = 150) -> GameState:
    parts = fen.split()
    if len(parts) < 2:
        raise ValueError(f"bad fen: {fen!r}")
    rows = parts[0].split("/")
    if len(rows) != 5:
        raise ValueError(f"bad fen rows: {fen!r}")
    game = GameState(idle_limit=None, max_moves=max_moves)
    game.board = [[None for _ in range(5)] for _ in range(5)]
    for r, row in enumerate(rows):
        c = 0
        for ch in row:
            if ch.isdigit():
                c += int(ch)
            elif ch == "w":
                game.board[r][c] = WOLF
                c += 1
            elif ch == "s":
                game.board[r][c] = SHEEP
                c += 1
            else:
                raise ValueError(f"bad fen char {ch!r} in {fen!r}")
        if c != 5:
            raise ValueError(f"bad fen row width in {fen!r}")
    game.turn = WOLF if parts[1] == "w" else SHEEP
    game.move_count = int(parts[2]) if len(parts) > 2 else 0
    return game


# ============================================================
# 对局会话（单局；刷新页面不丢局面）
# ============================================================
def game_to_fen(game: GameState) -> str:
    rows = []
    for r in range(5):
        row = ""
        empty = 0
        for c in range(5):
            p = game.board[r][c]
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
    return "/".join(rows) + " " + ("w" if game.turn == WOLF else "s") + " " + str(game.move_count)


class Session:
    def __init__(self, tb: Tablebase):
        self.tb = tb
        self.player_side = None  # None = 尚未选择执棋方
        self.endless = False     # 无尽模式：勾选后解除 150 步上限，超过仍继续提示/记录
        self.game = self._new_game()
        self.history = [game_to_fen(self.game)]
        self.log = []            # 对局记录（每步一行，含走完后局面的最优解结论）
        self.model_move = None   # ((r1,c1),(r2,c2)) 最近一次"已经执行"的模型走子
        self.model_capture = None
        self.model_note = ""
        self.model_pending = False  # 玩家刚走完，模型等待 advance 才应手
        self.record_saved = False   # 当前局是否已自动存档（终局/重开只存一次）
        self.last_saved = None      # 最近一次存档文件名

    def _new_game(self) -> GameState:
        """按当前无尽模式开关新建一局（无尽 → 不设步数上限）。"""
        return GameState(idle_limit=None, max_moves=None if self.endless else 150)

    # ---------- 自动存档 ----------
    def save_record(self) -> str | None:
        """把当前对局记录写入 data/saved_games/game_*.json，返回文件名或 None。

        记录为一次性 JSON：时间、执棋方、结果、步数、终局 FEN，以及每一步的
        完整对局记录（含吃子、走完后的表库最优解结论），便于回放与分析。
        """
        if not self.log:
            return None
        SAVED_DIR.mkdir(parents=True, exist_ok=True)
        g = self.game
        if g.winner == WOLF:
            result = "狼胜"
            reason = "羊被吃到不足 4 只"
        elif g.winner == SHEEP:
            result = "羊胜"
            reason = "三只狼均无法移动"
        elif g.winner == DRAW:
            result = "和棋"
            reason = "双方合计 150 步"
        else:
            result = "未完成"
            reason = "手动重开"
        rid = time.strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:6]
        record = {
            "id": rid,
            "saved_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "player_side": ("wolf" if self.player_side == WOLF else
                            "sheep" if self.player_side == SHEEP else None),
            "endless": self.endless,
            "result": result,
            "reason": reason,
            "move_count": g.move_count,
            "final_fen": game_to_fen(g),
            "moves": self.log,
        }
        name = f"game_{rid}.json"
        path = SAVED_DIR / name
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
        self.record_saved = True
        self.last_saved = name
        return name

    def saved_count(self) -> int:
        try:
            return len(list(SAVED_DIR.glob("game_*.json")))
        except OSError:
            return 0

    # ---------- 引擎：选一步 ----------
    def phase(self) -> str:
        # 纯表库引擎：开局羊数 15 ≤ 已解出最大 k（现为 15），整局都被覆盖
        return "tb"

    def choose_model(self):
        """为当前回合方选一步；返回 (start, dest, note) 或 None。"""
        choice = self.tb.best_move(self.game)
        if choice is None:
            return None, None, f"硬解表库 k={self.game.sheep_count} 未求解"
        (r1, c1), mv = choice
        return (r1, c1), mv.destination, f"硬解表库 k={self.game.sheep_count} 最优解"

    # ---------- 走子 ----------
    def _push_log(self, side, r1, c1, r2, c2, captured_idx):
        self.log.append({
            "n": self.game.move_count,
            "side": side,
            "me": side == ("wolf" if self.player_side == WOLF else "sheep"),
            "from": r1 * 5 + c1,
            "to": r2 * 5 + c2,
            "captured": captured_idx,
            "chip": self._chip(),
            "fen": game_to_fen(self.game),
        })

    def step_model(self):
        """执行一次模型走子（advance/换边/重开/悔棋后轮到模型时调用）。"""
        if self.game.winner is not None or self.game.turn == self.player_side:
            return False
        picked = self.choose_model()
        if picked is None or len(picked) < 3:
            return False
        (r1, c1), (r2, c2), note = picked
        side = "wolf" if self.game.turn == WOLF else "sheep"
        sheep_before = self.game.sheep_count
        if not self.game.move((r1, c1), (r2, c2)):
            return False
        captured = (r2 * 5 + c2) if self.game.sheep_count < sheep_before else None
        self.model_move = ((r1, c1), (r2, c2))
        self.model_capture = captured
        self.model_note = note
        self._push_log(side, r1, c1, r2, c2, captured)
        self.history.append(game_to_fen(self.game))
        self.model_pending = False
        return True

    def advance(self):
        """网页端隔 1 秒后调用：让模型完成应手。"""
        if (self.model_pending and self.game.winner is None
                and self.game.turn != self.player_side):
            self.model_pending = False
            self.step_model()

    # ---------- 操作 ----------
    def choose_side(self, side: str):
        # 若正在进行的对局还没存档，先存档再开新局（防止刷新页面/换边丢弃记录）
        if self.log and not self.record_saved:
            self.save_record()
        self.player_side = WOLF if side == "wolf" else SHEEP
        self.game = self._new_game()
        self.history = [game_to_fen(self.game)]
        self.log = []
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True  # 若你执羊，模型(狼)先行，1 秒后由 advance 走第一步
        self.record_saved = False
        self.last_saved = None

    def move(self, fr, to):
        if self.game.winner is not None:
            return False, "对局已结束"
        if self.game.turn != self.player_side:
            return False, "还没轮到你的回合"
        fr_c = (fr // 5, fr % 5)
        to_c = (to // 5, to % 5)
        side = "wolf" if self.game.turn == WOLF else "sheep"
        sheep_before = self.game.sheep_count
        if not self.game.move(fr_c, to_c):
            return False, "非法走法"
        captured = (to_c[0] * 5 + to_c[1]) if self.game.sheep_count < sheep_before else None
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self._push_log(side, fr_c[0], fr_c[1], to_c[0], to_c[1], captured)
        self.history.append(game_to_fen(self.game))   # 与 step_model 对齐：每走一步记一条
        self.model_pending = True
        return True, None

    def undo(self):
        if len(self.history) < 2:
            return False, "没有可悔的棋"
        self.history.pop()
        max_moves = self.game.max_moves
        self.game = game_from_fen(self.history[-1], max_moves=max_moves)
        # 对局记录按“步数序”截断：每步恰好一条日志、一条 history FEN，严格对齐，
        # 不再按 FEN 集合匹配（同局面复现/玩家行棋不入 history 会导致误删整段记录）。
        self.log = self.log[:len(self.history) - 1]
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True
        return True, None

    def switch(self):
        if self.player_side is None:
            return False, "请先选择执棋方"
        self.player_side = SHEEP if self.player_side == WOLF else WOLF
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True
        return True, None

    def set_endless(self, on: bool):
        """开/关无尽模式：解除 150 步上限；若已因 150 步判和，解除终局继续下；
        若已超过 150 步后关掉无尽模式，立即按 150 步上限判和。"""
        self.endless = bool(on)
        self.game.max_moves = None if self.endless else 150
        if on and self.game.winner == DRAW:
            self.game.winner = None
            self.model_pending = True
        elif not on and self.game.winner is None and self.game.move_count >= 150:
            self.game.winner = DRAW
        return True, None

    def restart(self):
        # 手动重开前把未存档的当前局自动保存（终局局已在 snapshot 里存过则跳过）
        if not self.record_saved:
            self.save_record()
        self.game = self._new_game()
        self.history = [game_to_fen(self.game)]
        self.log = []
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True
        self.record_saved = False
        return True, None

    # ---------- 分析 ----------
    def _chip(self) -> str:
        """当前局面的结论（用于对局记录里每步后的最优解 chip）。"""
        g = self.game
        if g.winner == WOLF:
            return "狼胜·终局"
        if g.winner == SHEEP:
            return "羊胜·终局"
        if g.winner == DRAW:
            return "和棋·终局"
        if not self.tb._open(g.sheep_count):
            return f"表库 k={g.sheep_count}"
        known, result, dist = self.tb.lookup(g)
        if not known:
            return f"表库 k={g.sheep_count}"
        return move_label(result, dist)

    def legal_list(self) -> list:
        out = []
        g = self.game
        for r in range(5):
            for c in range(5):
                if g.board[r][c] != g.turn:
                    continue
                for mv in g.legal_moves_from((r, c)):
                    out.append({
                        "from": r * 5 + c,
                        "to": mv.destination[0] * 5 + mv.destination[1],
                        "captured": (mv.captured[0] * 5 + mv.captured[1]) if mv.captured else None,
                    })
        return out

    def analysis(self, limit: int = 10):
        """当前回合方的候选最优续着（推给网页右侧“多条最优解”面板，默认 10 条）。

        展示池 = 全部合法走法（对模型有利/不利的都列出）；
        排序：对当前回合方有利 → 步数最少者优先；其次和棋；最后不利方（拖得越久越优先）。
        轮到模型走时，用 choose_model 的同一套规则挑选“对它有利且最快步数最少”的一步
        （全部来自表库最优解），并在对应行打 exec 标记。
        """
        g = self.game
        if g.winner is not None:
            return None
        legal = self.legal_list()
        if not legal:
            return None
        # 轮到模型走：预先算出它将执行的一步（与真实走子完全同源）
        exec_from = exec_to = None
        if self.player_side is not None and g.turn != self.player_side:
            picked = self.choose_model()
            if picked is not None and len(picked) >= 3 and picked[0] is not None:
                exec_from = picked[0][0] * 5 + picked[0][1]
                exec_to = picked[1][0] * 5 + picked[1][1]
        rows = None
        if not self.tb._open(g.sheep_count):
            return None
        my_win = hsf.WOLF_WIN if g.turn == WOLF else hsf.SHEEP_WIN
        scored = []
        for m in legal:
            trial = copy.deepcopy(g)
            if not trial.move((m["from"] // 5, m["from"] % 5), (m["to"] // 5, m["to"] % 5)):
                continue
            known, result, dist = self.tb.lookup(trial)
            if not known:
                continue
            rank = 2 if result == my_win else (1 if result == hsf.DRAW else 0)
            sec = -dist if rank == 2 else (dist if rank == 0 else 0)
            scored.append((rank, sec, dict(m, label=move_label(result, dist))))
        scored.sort(key=lambda x: (x[0], x[1]), reverse=True)
        rows = [s[2] for s in scored[:limit]]
        if rows and exec_from is not None:
            for r in rows:
                if r["from"] == exec_from and r["to"] == exec_to:
                    r["exec"] = True
                    break
        return rows

    # ---------- 输出 ----------
    def snapshot(self) -> dict:
        g = self.game
        # 对局结束 → 自动保存对局记录（仅存一次）
        if g.winner is not None and not self.record_saved:
            self.save_record()
        board = [g.board[i // 5][i % 5] for i in range(25)]
        verdict = {"known": False, "label": ""}
        known, result, dist = self.tb.lookup(g)
        if known:
            verdict = {"known": True, "label": move_label(result, dist)}
        else:
            verdict = {"known": False, "label": f"表库 k={g.sheep_count} 未求解"}
        terminal = None
        if g.winner == WOLF:
            terminal = {"result": "狼胜", "reason": "羊被吃到不足 4 只"}
        elif g.winner == SHEEP:
            terminal = {"result": "羊胜", "reason": "三只狼均无法移动"}
        elif g.winner == DRAW:
            terminal = {"result": "和棋", "reason": "双方合计 150 步"}
        return {
            "board": board,
            "turn": "wolf" if g.turn == WOLF else "sheep",
            "player_side": ("wolf" if self.player_side == WOLF else
                            "sheep" if self.player_side == SHEEP else None),
            "endless": self.endless,
            "record_saved": self.record_saved,
            "last_saved": self.last_saved,
            "saved_count": self.saved_count(),
            "sheep": g.sheep_count,
            "move_count": g.move_count,
            "phase": self.phase(),
            "model_to_move": (self.player_side is not None and
                              g.turn != self.player_side and g.winner is None),
            "model_pending": self.model_pending,
            "model_move": ({"from": self.model_move[0][0] * 5 + self.model_move[0][1],
                            "to": self.model_move[1][0] * 5 + self.model_move[1][1],
                            "captured": self.model_capture}
                           if self.model_move else None),
            "model_note": self.model_note,
            "verdict": verdict,
            "terminal": terminal,
            "legal": self.legal_list(),
            "analysis": self.analysis(),
            "log": self.log,
            "history_len": len(self.history),
        }


# ============================================================
# HTTP 服务
# ============================================================
session = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 精简日志
        print("[web] " + fmt % args, flush=True)

    def _send(self, code, body: bytes, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            try:
                self._send(200, INDEX_PATH.read_bytes(), "text/html; charset=utf-8")
            except OSError:
                self._send(404, b"index.html not found", "text/plain")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path != "/api":
            self._send(404, b"not found", "text/plain")
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            data = json.loads(self.rfile.read(length) or b"{}")
        except (ValueError, json.JSONDecodeError):
            self._send(400, json.dumps({"ok": False, "error": "bad json"}).encode())
            return
        cmd = data.get("cmd", "state")
        global session
        try:
            if cmd == "choose":
                session.choose_side(data.get("side", "wolf"))
            elif cmd == "move":
                ok, err = session.move(data.get("from"), data.get("to"))
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd == "advance":
                session.advance()
            elif cmd == "undo":
                ok, err = session.undo()
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd == "switch":
                ok, err = session.switch()
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd == "restart":
                session.restart()
            elif cmd == "endless":
                ok, err = session.set_endless(bool(data.get("on", False)))
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd != "state":
                self._send(400, json.dumps({"ok": False, "error": f"unknown cmd {cmd}"}).encode())
                return
            body = json.dumps({"ok": True, "state": session.snapshot()}, ensure_ascii=False).encode()
            self._send(200, body)
        except Exception as exc:  # noqa: BLE001
            print(f"[web] api error: {exc}", flush=True)
            self._send(500, json.dumps({"ok": False, "error": str(exc)}).encode())


def main():
    ap = argparse.ArgumentParser(description="狼羊棋 网页版后端")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--data-dir", default=str(ROOT / "data" / "ws_tb_dtc_260819"))
    args = ap.parse_args()

    global session
    tb = Tablebase(args.data_dir)
    session = Session(tb)
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("=== 狼羊棋 · 网页版 ===", flush=True)
    print(f"本机访问: http://{args.host}:{args.port}", flush=True)
    print(f"远端开发(ssh 端口转发): ssh -L {args.port}:127.0.0.1:{args.port} <user>@<host>", flush=True)
    print(f"硬解表库: {args.data_dir}（已截完 k ≤ {tb.max_k}；开局羊数 15 全程在表库覆盖内）", flush=True)
    print("引擎: 表库纯最优解（无神经网络依赖）", flush=True)
    print("Ctrl-C 退出。", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n再见。")


if __name__ == "__main__":
    main()