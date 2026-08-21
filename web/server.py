#!/usr/bin/env python3
"""web/server.py — 狼羊棋 · 网页版后端（纯 Python 标准库，无第三方依赖）

- POST /api：choose / move / advance / undo / switch / restart / endless / mode / free / model_move / state / export_lines
- 自动存档：对局结束或手动重开时，自动把对局记录落盘到 data/saved_games/*.json
  （文件名简化：去掉开头的 game_ 前缀与世纪的“20”，保留两位年份，形如
  260821_134920_31fe75.json；JSON 含每步走子、吃子与走完后的表库最优解结论，便于回放分析）。
- 无尽模式（endless）：勾选后解除 150 步上限，超过后仍继续对局，
  最优解提示与对局记录不受影响（表库结论只依赖局面，与步数无关）。
- 手动模式（mode/manual_move）：开 → 模型不自动应手，右侧给出推荐最优解，
  由玩家点选一条替模型走棋；关（默认）→ 模型自动应手。
- 自动/手动模式下，轮到模型走棋时**都可以**点右侧候选最优解替模型走棋
  （自动模式下点选即覆盖即将执行的自动应手），玩家自己点棋子走棋两种模式不变。
- 模型选步（防 5 次重复判和）：只在表库最优档选步（必胜最快/必败拖延/和棋），
  同档内优先选不会回到已出现局面的走法（防来回走），剩余等价走法随机挑一个。
- 交互节奏：玩家走子后服务端**不立即让模型应手**（响应里 model_to_move=true），
  网页端等待 1 秒后再调 advance，模型才走子（符合“玩家行棋后隔一秒再由模型走棋”）。
- 每步响应包含：
    analysis   —— 当前回合方的全部候选最优续着（表库结论，不截断展示）
    log        —— 对局记录：每一步(玩家/模型)的走子 + 走完后局面的最优解结论；
                 by 标记来源：ai=模型自动应手(最优解) / opt=玩家代走且属最优解 / self=玩家自选
    legal      —— 当前回合方全部合法走法（网页点选高亮用）
    verdict    —— 当前局面最优解（狼胜/羊胜/和棋 + 最快步数）
- **一键导出最优棋谱（export_lines）**：基于当前局面，为当前走棋方导出 ≤3 条
  “赢或和”的完整最优棋谱（每条含每步走棋前的棋盘布局 + 起子/落点/吃子，供前端按
  网页“鼠标悬浮最优解”的高亮效果渲染 5 列多行高清子图）。棋谱按“对手最强防守”
  生成：对手每步都走表库最优防守（最长拖延、绝不送分），并在官方规则下实测校验
  终局（必须真实走到“获胜方胜”或“和棋”）。计算期间 HTTP 层加锁，禁止任何其他
  操作；导出对局只读，不改动当前局面。
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
import random
import re
import sys
import threading
import time
import uuid
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))                              # hard_solve_fast
sys.path.insert(0, str(ROOT / "wolves_eat_sheep_game"))    # rules
sys.path.insert(0, str(Path(__file__).resolve().parent))   # exporter

BJ_TZ = timezone(timedelta(hours=8))   # 北京时间（UTC+8，无夏令时）


def now_str(fmt: str) -> str:
    """按北京时间取当前时间字符串。

    服务器可能运行在 UTC 时区（本地 time.strftime 会早 8 小时），
    存档时间/文件名一律以北京时间为准，与进程时区无关。
    """
    return datetime.now(BJ_TZ).strftime(fmt)

from rules import GameState, WOLF, SHEEP, DRAW, IDLE_LIMIT  # noqa: E402
import hard_solve_fast as hsf  # noqa: E402
import exporter  # noqa: E402

INDEX_PATH = Path(__file__).parent / "index.html"
SAVED_DIR = ROOT / "data" / "saved_games"   # 自动保存的对局记录（JSON 文件）

TB_RESULT_LABEL = {hsf.WOLF_WIN: "狼胜", hsf.SHEEP_WIN: "羊胜", hsf.DRAW: "和棋", hsf.UNKNOWN: "未知"}


def move_label(result: int, dist: int) -> str:
    """结论标签：狼胜·最快9步 / 羊胜·最快12步 / 和棋"""
    lab = TB_RESULT_LABEL.get(result, "未知")
    if result in (hsf.WOLF_WIN, hsf.SHEEP_WIN):
        lab += f"·最快{dist}步"
    return lab


def draw_reason(g: GameState) -> str:
    """和棋原因：达到步数上限 / 相同局面重复出现（官方 5 次判和规则）。"""
    if g.max_moves is not None and g.move_count >= g.max_moves:
        return f"双方合计 {g.max_moves} 步"
    return "相同局面出现 5 次"


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
def game_from_fen(fen: str, max_moves: int | None = 150, idle_limit: int | None = IDLE_LIMIT) -> GameState:
    parts = fen.split()
    if len(parts) < 2:
        raise ValueError(f"bad fen: {fen!r}")
    rows = parts[0].split("/")
    if len(rows) != 5:
        raise ValueError(f"bad fen rows: {fen!r}")
    game = GameState(idle_limit=idle_limit, max_moves=max_moves)
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


def book_key(cells, turn) -> str:
    """开局库局面 key：25 格按行拼串（w/s/.）+ 回合（w/s）。与 tools/opening_book.py
    生成的开局库 JSON 的 key 完全一致；cells 为 25 长度可迭代对象。"""
    return "".join(("w" if c == WOLF else "s" if c == SHEEP else ".") for c in cells) \
        + ":" + ("w" if turn == WOLF else "s")


BOOK_PATH = ROOT / "web" / "opening_book.json"


class Session:
    def __init__(self, tb: Tablebase):
        self.tb = tb
        self.player_side = None  # None = 尚未选择执棋方
        self.endless = False     # 无尽模式：勾选后解除 150 步上限，超过仍继续提示/记录
        self.manual = False      # 手动模式：模型给出最优解，由玩家点选替模型走棋
        self.free = False        # 自由移动（摆子）：狼任意跳/跳吃任意羊、羊任意空地，无步数；不提供最优解
        self.game = self._new_game()
        self.history = [game_to_fen(self.game)]
        self.log = []            # 对局记录（每步一行，含走完后局面的最优解结论）
        self.model_move = None   # ((r1,c1),(r2,c2)) 最近一次"已经执行"的模型走子
        self.model_capture = None
        self.model_note = ""
        self.model_pending = False  # 玩家刚走完，模型等待 advance 才应手
        self.record_saved = False   # 当前局是否已自动存档（终局/重开只存一次）
        self.last_saved = None      # 最近一次存档文件名
        self._seen = {self._pos_key(self.game)}  # 本局出现过的局面 key（模型防重复用）
        self._pick_cache = None     # 同一局面内模型选步缓存（(fen, choice)）
        self.opening_book = None    # 开局库：web/opening_book.json（前 book_max_plies 个半回合）
        self.book_max_plies = 0
        self.book_hits = 0          # 本局开局库命中次数（日志/统计用）
        try:
            if BOOK_PATH.exists():
                with open(BOOK_PATH, encoding="utf-8") as f:
                    bj = json.load(f)
                self.opening_book = bj.get("positions") or {}
                self.book_max_plies = int(bj.get("max_plies", 0) or 0)
        except (OSError, ValueError):
            self.opening_book = None
            self.book_max_plies = 0

    def _new_game(self) -> GameState:
        """新建一局（官方规则含 5 次重复判和；无尽/自由移动 → 不设步数上限）。"""
        g = GameState(idle_limit=None if self.free else IDLE_LIMIT,
                      max_moves=None if (self.endless or self.free) else 150)
        g.free_moves = self.free
        return g

    @staticmethod
    def _pos_key(game: GameState):
        """局面唯一 key（棋盘 + 轮到谁走），用于模型“不回到已出现的局面”的约束。"""
        return (tuple(game.board[i // 5][i % 5] for i in range(25)), game.turn)

    def _rebuild_seen(self) -> None:
        """悔棋后按 history（每步一个 FEN）重建已出现局面集合。"""
        self._seen = {
            self._pos_key(game_from_fen(f, max_moves=self.game.max_moves,
                                        idle_limit=self.game.idle_limit))
            for f in self.history
        }

    # ---------- 自动存档 ----------
    def save_record(self) -> str | None:
        """把当前对局记录写入 data/saved_games/*.json，返回文件名或 None。

        记录为一次性 JSON：时间、执棋方、结果、步数、终局 FEN，以及每一步的
        完整对局记录（含吃子、走完后的表库最优解结论），便于回放与分析。
        文件名从“game_YYYYMMDD_HHMMSS_xxxxxx”简化为“YYMMDD_HHMMSS_xxxxxx”
        （去掉开头的 game_ 前缀与世纪的“20”，保留两位年份；完整时间仍存于
        saved_at 字段，一律按北京时间 UTC+8 记录）。
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
            reason = draw_reason(g)
        else:
            result = "未完成"
            reason = "手动重开"
        rid = now_str("%y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:6]
        # rid = YYMMDD_HHMMSS_xxxxxx（北京时间）：去掉开头 game_ 前缀与世纪“20”，保留两年份
        record = {
            "id": rid,
            "saved_at": now_str("%Y-%m-%d %H:%M:%S"),
            "player_side": ("wolf" if self.player_side == WOLF else
                            "sheep" if self.player_side == SHEEP else None),
            "endless": self.endless,
            "manual": self.manual,
            "free": self.free,
            "result": result,
            "reason": reason,
            "move_count": g.move_count,
            "final_fen": game_to_fen(g),
            "moves": self.log,
        }
        name = f"{rid}.json"
        path = SAVED_DIR / name
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
        self.record_saved = True
        self.last_saved = name
        return name

    def saved_count(self) -> int:
        try:
            return len(list(SAVED_DIR.glob("*.json")))
        except OSError:
            return 0

    # ---------- 引擎：选一步 ----------
    def phase(self) -> str:
        # 纯表库引擎：开局羊数 15 ≤ 已解出最大 k（现为 15），整局都被覆盖
        return "tb"

    def _model_choice(self) -> dict | None:
        """在表库最优解里做“规则性 + 随机性”选步。

        规则性（防 5 次重复判和）：
          - 只考虑当前回合方最优的一档（必胜取最快、必败取拖延最久、和棋取和）；
          - 同档走法中，优先选不会回到本局已出现过局面的走法；
            （回合制循环/来回走必然复现旧局面 → 被挡掉，避免模型把能赢的棋走和）
          - 再优先选不会让所动棋子“来回摆动”的走法（直接避开官方“同子反复 5 次
            判和”的判定路径；来回摆动的棋子 back-and-forth 计数 > 0）。
        随机性：
          - 在满足上述规则的等价最优走法里均匀随机挑一个，避免永远走同一招。
        """
        g = self.game
        my_win = hsf.WOLF_WIN if g.turn == WOLF else hsf.SHEEP_WIN
        cands = []
        for r in range(5):
            for c in range(5):
                if g.board[r][c] != g.turn:
                    continue
                for mv in g.legal_moves_from((r, c)):
                    trial = copy.deepcopy(g)
                    if not trial.move((r, c), mv.destination):
                        continue
                    known, result, dist = self.tb.lookup(trial)
                    if not known:
                        continue
                    moved_id = trial.piece_ids.get(mv.destination)
                    osc = (trial._back_and_forth_count(trial.piece_histories[moved_id])
                           if moved_id in trial.piece_histories else 0)
                    cands.append({
                        "from": (r, c),
                        "to": mv.destination,
                        "rank": 2 if result == my_win else (1 if result == hsf.DRAW else 0),
                        "dist": dist,
                        "osc": osc,
                        "key": self._pos_key(trial),
                    })
        if not cands:
            return None
        cands.sort(key=lambda m: (m["rank"], -m["dist"]), reverse=True)
        best_rank = cands[0]["rank"]
        if best_rank == 2:
            best_dist = cands[0]["dist"]
            pool = [m for m in cands if m["rank"] == 2 and m["dist"] == best_dist]
        elif best_rank == 1:
            pool = [m for m in cands if m["rank"] == 1]
        else:
            best_dist = max(m["dist"] for m in cands)
            pool = [m for m in cands if m["dist"] == best_dist]
        fresh = [m for m in pool if m["key"] not in self._seen]
        sel = fresh if fresh else pool
        min_osc = min(m["osc"] for m in sel)
        sel = [m for m in sel if m["osc"] == min_osc]
        # 开局库：开局前几个半回合（move_count < book_max_plies）按书中优选序定点选步——
        # 必胜取最快、和棋保和、必败拖最久，且同档不再随机摇摆 → 开局走法稳定精准。
        node = None
        if self.opening_book and g.move_count < self.book_max_plies:
            node = self.opening_book.get(book_key(
                (g.board[i // 5][i % 5] for i in range(25)), g.turn))
        if node and node.get("m"):
            order = {(m[0], m[1]): i for i, m in enumerate(node["m"])}
            cand = [m for m in sel
                    if (m["from"][0] * 5 + m["from"][1], m["to"][0] * 5 + m["to"][1]) in order]
            if cand:
                chosen = min(cand, key=lambda m: order[
                    (m["from"][0] * 5 + m["from"][1], m["to"][0] * 5 + m["to"][1])])
                chosen["book"] = True
                self.book_hits += 1
                return chosen
        chosen = random.choice(sel)
        chosen["book"] = False
        return chosen

    def choose_model(self):
        """为当前回合方选一步；返回 (start, dest, note) 或 None。

        同一局面缓存结果：前端“模型将走”提示与实际执行的一步必须一致。
        """
        fen = game_to_fen(self.game)
        if self._pick_cache is not None and self._pick_cache[0] == fen:
            choice = self._pick_cache[1]
        else:
            choice = self._model_choice()
            self._pick_cache = (fen, choice)
        if choice is None:
            return None, None, f"硬解表库 k={self.game.sheep_count} 未求解"
        note = f"硬解表库 k={self.game.sheep_count} 最优解"
        if choice.get("book"):
            note += "（开局库）"
        return choice["from"], choice["to"], note

    def is_optimal_move(self, fr: int, to: int) -> bool:
        """该走法 (fr,to) 是否属于当前局面下当前回合方的“最优解档”。

        （用于日志标记：玩家帮模型走棋时，走的是最优解还是自选棋。与
        _model_choice 的最优档判定一致：必胜取最快 / 和棋取和 / 必败拖延最久。）
        """
        g = self.game
        my_win = hsf.WOLF_WIN if g.turn == WOLF else hsf.SHEEP_WIN
        cands = []
        for r in range(5):
            for c in range(5):
                if g.board[r][c] != g.turn:
                    continue
                for mv in g.legal_moves_from((r, c)):
                    trial = copy.deepcopy(g)
                    if not trial.move((r, c), mv.destination):
                        continue
                    known, result, dist = self.tb.lookup(trial)
                    if not known:
                        continue
                    rank = 2 if result == my_win else (1 if result == hsf.DRAW else 0)
                    cands.append((rank, dist, (r * 5 + c, mv.destination[0] * 5 + mv.destination[1])))
        if not cands:
            return False
        best_rank = max(c[0] for c in cands)
        target = (fr, to)
        if not any(c[2] == target for c in cands):
            return False
        if best_rank == 1:                     # 和棋档：任何和棋走法都算最优解
            return any(c[0] == 1 and c[2] == target for c in cands)
        best_dist = (min if best_rank == 2 else max)(
            c[1] for c in cands if c[0] == best_rank)
        return any(c[0] == best_rank and c[1] == best_dist and c[2] == target for c in cands)

    # ---------- 最优棋谱导出（只读；计算期间由 HTTP 层加锁禁止其他操作） ----------
    def export_lines(self, count: int = 1, champion: str | None = None, target: str | None = None,
                     max_steps: int | None = None, progress=None) -> dict:
        """为指定视角导出 1～10 条“赢或和”最优棋谱（完整线 + 每步棋盘布局）。

        champion: 'wolf'/'sheep' 先走方视角（缺省取当前轮到方）；target: 'wolf_win'/
                  'sheep_win'/'draw' 目标结局（缺省取相应视角的表库最优结局）；
        max_steps: 用户设定的最大总步数上限（超出该值的最优解不进棋谱）；
        progress:  进度回调 progress(pct, phase)，用于加载界面进度条/耗时显示。
        对局完全只读：不改动当前局面/历史/存档。计算可能耗时较长（数秒～数分钟），
        HTTP 处理器在计算期间会拒绝所有其他指令。
        """
        if self.free:
            return {"ok": False, "error": "自由移动（摆子）模式基于变体规则，不提供棋谱导出"}
        if self.game.winner is not None:
            return {"ok": False, "error": "对局已结束，没有可导出的最优棋谱"}
        if self.player_side is None:
            return {"ok": False, "error": "请先选择执棋方后再导出"}
        return exporter.run_export(self.tb, self.game, count=count, endless=self.endless,
                                   champion=champion, target=target,
                                   max_steps=max_steps, progress=progress)

    # ---------- 走子 ----------
    def _push_log(self, side, r1, c1, r2, c2, captured_idx, by=None):
        self.log.append({
            "n": self.game.move_count,
            "side": side,
            "me": side == ("wolf" if self.player_side == WOLF else "sheep"),
            "from": r1 * 5 + c1,
            "to": r2 * 5 + c2,
            "captured": captured_idx,
            "by": by,     # ai=模型自动应手(最优解) / opt=玩家代走且属最优解 / self=玩家自选
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
        self._push_log(side, r1, c1, r2, c2, captured, by="ai")
        self.history.append(game_to_fen(self.game))
        self._seen.add(self._pos_key(self.game))
        self._pick_cache = None
        self.model_pending = False
        return True

    def advance(self):
        """网页端隔 1 秒后调用：让模型完成应手（手动/自由移动模式下不做自动应手）。"""
        if self.manual or self.free:
            return
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
        self._seen = {self._pos_key(self.game)}
        self._pick_cache = None

    def move(self, fr, to):
        if self.game.winner is not None:
            return False, "对局已结束"
        # 自由移动模式：任何一方棋子都可被直接点选移动，不受回合限制
        if not self.free and self.game.turn != self.player_side:
            return False, "还没轮到你的回合"
        fr_c = (fr // 5, fr % 5)
        to_c = (to // 5, to % 5)
        side = self.game.board[fr_c[0]][fr_c[1]]
        if side is None:
            return False, "非法走法"
        if self.free:
            self.game.turn = side       # 自由摆子：临时轮到被走子所属方
        side_str = "wolf" if side == WOLF else "sheep"
        sheep_before = self.game.sheep_count
        if not self.game.move(fr_c, to_c):
            return False, "非法走法"
        captured = (to_c[0] * 5 + to_c[1]) if self.game.sheep_count < sheep_before else None
        if self.free:
            self.game.turn = side       # 保持走子方（自由模式下回合仅作显示）
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self._push_log(side_str, fr_c[0], fr_c[1], to_c[0], to_c[1], captured)
        self.history.append(game_to_fen(self.game))   # 与 step_model 对齐：每走一步记一条
        self._seen.add(self._pos_key(self.game))
        self.model_pending = True
        return True, None

    def undo(self):
        if len(self.history) < 2:
            return False, "没有可悔的棋"
        self.history.pop()
        max_moves = self.game.max_moves
        idle_limit = self.game.idle_limit
        self.game = game_from_fen(self.history[-1], max_moves=max_moves, idle_limit=idle_limit)
        self.game.free_moves = self.free   # 自由移动模式重建局面后恢复变体开关
        # 对局记录按“步数序”截断：每步恰好一条日志、一条 history FEN，严格对齐，
        # 不再按 FEN 集合匹配（同局面复现/玩家行棋不入 history 会导致误删整段记录）。
        self.log = self.log[:len(self.history) - 1]
        self._rebuild_seen()
        self._pick_cache = None
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True
        return True, None

    def switch(self):
        if self.player_side is None:
            return False, "请先选择执棋方"
        self.player_side = SHEEP if self.player_side == WOLF else WOLF
        self._pick_cache = None
        self.model_move = None
        self.model_capture = None
        self.model_note = ""
        self.model_pending = True
        return True, None

    def set_endless(self, on: bool):
        """开/关无尽模式：解除 150 步上限；若已因 150 步判和，解除终局继续下；
        若已超过 150 步后关掉无尽模式，立即按 150 步上限判和。"""
        self.endless = bool(on)
        self.game.max_moves = None if (self.endless or self.free) else 150
        if on and self.game.winner == DRAW:
            self.game.winner = None
            self.model_pending = True
        elif not on and self.game.winner is None and self.game.move_count >= 150:
            self.game.winner = DRAW
        return True, None

    def set_manual(self, on: bool):
        """开/关手动模式：开 → 模型不再自动应手，由玩家点选右侧推荐走法替模型走棋。

        自由移动开启期间强制手动模式，无法自行切回自动模式。
        """
        if self.free and not on:
            return False, "自由移动模式下为手动模式，无法切换回自动模式（请先关闭自由移动）"
        self.manual = bool(on)
        self._pick_cache = None
        return True, None

    def set_free(self, on: bool):
        """开/关自由移动（摆子）模式。

        开启：狼可跳到任意位置或任意羊格跳吃、羊可移动到任意空地，无步数限制，
        玩家可直接点选**双方任意棋子**快速摆放/调整残局；该模式**不提供最优解更新**
        （表库按标准规则求解，不适用该变体），并**自动切到手动模式**（期间无法切回
        自动模式），避免模型自动应手破坏摆好的局面。关闭后保持手动模式，玩家可自行
        切回自动模式；关闭即按当前局面恢复标准规则与最优解提示。
        """
        self.free = bool(on)
        if on:
            self.manual = True          # 自由移动期间强制手动（关闭后保持手动，可自行切回自动）
        self.game.free_moves = on
        self.game.idle_limit = None if on else IDLE_LIMIT
        self.game.max_moves = None if (on or self.endless) else 150
        self._pick_cache = None
        return True, None

    def model_move_cmd(self, fr, to):
        """玩家点选/棋盘点击替模型走棋（自动/手动模式均可；自动模式下点选即覆盖自动应手）。

        自由移动模式下：无论轮到哪方都可点选任意棋子自由走（同 move）。
        日志标记 by：所走棋属于当前局面“最优解档”→ opt（按最优解标记），
        否则 → self（玩家自选）；模型自动应手另标 ai；自由移动模式不标注。
        """
        if self.game.winner is not None:
            return False, "对局已结束"
        if not self.free and self.game.turn == self.player_side:
            return False, "现在轮到你走棋"
        fr_c = (fr // 5, fr % 5)
        to_c = (to // 5, to % 5)
        side = self.game.board[fr_c[0]][fr_c[1]]
        if side is None:
            return False, "非法走法"
        if self.free:
            self.game.turn = side       # 自由摆子：临时轮到被走子所属方
        side_str = "wolf" if side == WOLF else "sheep"
        is_best = False if self.free else self.is_optimal_move(fr, to)
        sheep_before = self.game.sheep_count
        if not self.game.move(fr_c, to_c):
            return False, "非法走法"
        captured = (to_c[0] * 5 + to_c[1]) if self.game.sheep_count < sheep_before else None
        if self.free:
            self.game.turn = side       # 保持走子方（自由模式下回合仅作显示）
        self.model_move = (fr_c, to_c)
        self.model_capture = captured
        self.model_note = ("自由移动·自由走" if self.free else
                           ("点选最优解代走" if is_best else "玩家自选走棋"))
        self._push_log(side_str, fr_c[0], fr_c[1], to_c[0], to_c[1], captured,
                       by=None if self.free else ("opt" if is_best else "self"))
        self.history.append(game_to_fen(self.game))
        self._seen.add(self._pos_key(self.game))
        self._pick_cache = None
        self.model_pending = False
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
        self._seen = {self._pos_key(self.game)}
        self._pick_cache = None
        return True, None

    # ---------- 分析 ----------
    def _chip(self) -> str:
        """当前局面的结论（用于对局记录里每步后的结论 chip；自由移动模式不读表库）。"""
        if self.free:
            return "自由移动"
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
        # 自由移动模式：列出双方全部合法走法（供自由摆子高亮），否则只列当前回合方
        sides = (WOLF, SHEEP) if self.free else (g.turn,)
        for side in sides:
            for r in range(5):
                for c in range(5):
                    if g.board[r][c] != side:
                        continue
                    moves = (g.legal_free_moves_from((r, c)) if self.free
                             else g.legal_moves_from((r, c)))
                    for mv in moves:
                        out.append({
                            "from": r * 5 + c,
                            "to": mv.destination[0] * 5 + mv.destination[1],
                            "captured": (mv.captured[0] * 5 + mv.captured[1]) if mv.captured else None,
                            "side": "wolf" if side == WOLF else "sheep",
                        })
        return out

    def analysis(self, limit: int | None = None):
        """当前回合方的全部候选最优续着（推给网页右侧“最优解”面板；limit=None 不截断）。

        展示池 = 全部合法走法（对模型有利/不利的都列出），不再截断前 10 条；
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
        # 轮到模型走：预先算出它将执行的一步（与真实走子完全同源；手动模式不标注）
        exec_from = exec_to = None
        if (self.player_side is not None and g.turn != self.player_side and not self.manual):
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
        rows = [s[2] for s in scored] if limit is None else [s[2] for s in scored[:limit]]
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
        # 自由移动模式：不提供最优解更新（表库按标准规则求解，不适用该变体）
        if self.free:
            verdict = {"known": False, "label": "自由移动模式"}
        else:
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
            terminal = {"result": "和棋", "reason": draw_reason(g)}
        return {
            "board": board,
            "turn": "wolf" if g.turn == WOLF else "sheep",
            "player_side": ("wolf" if self.player_side == WOLF else
                            "sheep" if self.player_side == SHEEP else None),
            "endless": self.endless,
            "manual": self.manual,
            "free": self.free,
            "record_saved": self.record_saved,
            "last_saved": self.last_saved,
            "saved_count": self.saved_count(),
            "sheep": g.sheep_count,
            "move_count": g.move_count,
            "phase": self.phase(),
            "model_to_move": (not self.free and self.player_side is not None and
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
            "analysis": None if self.free else self.analysis(),
            "log": self.log,
            "history_len": len(self.history),
        }


# ============================================================
# HTTP 服务
# ============================================================
session = None
# 棋谱导出计算期间：禁止任何其他操作（线程级加锁；计算可能耗时数秒～数分钟）
_EXPORTING = False
_EXPORT_LOCK = threading.Lock()
# 导出任务进度（前端 progress bar / 百分比 / 耗时）：由后台线程更新、export_status 轮询读取
_EXPORT_PROG = {"pct": 0.0, "phase": "准备中…", "done": False, "result": None}


def _export_job(session, data: dict, prog: dict) -> None:
    """后台线程执行导出计算；prog 为进度字典（pct/phase/done/result），线程安全地逐字段更新。"""
    global _EXPORTING
    t0 = time.time()
    try:
        prog["pct"] = 0.5
        prog["phase"] = "初始化…"
        res = session.export_lines(data.get("count", 1),
                                   champion=data.get("champion"),
                                   target=data.get("target"),
                                   max_steps=data.get("max_steps"),
                                   progress=lambda p, ph: (prog.update(pct=p, phase=ph)))
        if res.get("ok"):
            prog["result"] = {"ok": True, "export": res}   # 与前端轮询约定一致（含 ok/export/error）
        else:
            prog["result"] = {"ok": False, "error": res.get("error", "导出失败")}
    except Exception as exc:  # noqa: BLE001
        print(f"[web] export error: {exc}", flush=True)
        prog["result"] = {"ok": False, "error": f"导出出错：{exc}"}
    finally:
        prog["pct"] = 100.0
        prog["phase"] = "完成（{} {:.2f}s）".format("成功" if prog["result"].get("ok") else "失败",
                                                  time.time() - t0)
        prog["done"] = True
        _EXPORTING = False
        _EXPORT_LOCK.release()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 精简日志，与服务启动日志前缀一致
        print("[狼羊棋·web] " + fmt % args, flush=True)

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
        global session, _EXPORTING, _EXPORT_PROG
        if cmd == "export_lines":
            # 提交导出任务：后台线程计算并上报进度；POST 立即返回，前端轮询 export_status
            if _EXPORTING or not _EXPORT_LOCK.acquire(blocking=False):
                self._send(200, json.dumps(
                    {"ok": False, "error": "已有棋谱导出正在计算，请稍候再试"}).encode())
                return
            _EXPORTING = True
            prog = {"pct": 0.0, "phase": "准备中…", "done": False, "result": None}
            _EXPORT_PROG = prog
            threading.Thread(target=_export_job, args=(session, data, prog), daemon=True).start()
            body = json.dumps({"ok": True, "state": session.snapshot(), "exporting": True},
                              ensure_ascii=False).encode()
            self._send(200, body)
            return
        if cmd == "export_status":
            # 轮询导出进度/结果（计算期间允许；done 后携带完整结果）
            prog = _EXPORT_PROG
            if prog is None:
                body = {"ok": True, "done": True,
                        "result": {"ok": False, "error": "没有正在进行的导出任务"}}
            else:
                body = {"ok": True, "done": prog["done"],
                        "pct": round(prog["pct"], 1), "phase": prog["phase"]}
                if prog["done"]:
                    body["result"] = prog["result"]
            self._send(200, json.dumps(body, ensure_ascii=False).encode())
            return
        if _EXPORTING and cmd not in ("state", "export_status"):
            # 计算期间禁止任何操作；state/export_status 为只读轮询，允许
            self._send(200, json.dumps(
                {"ok": False, "error": "正在导出棋谱，计算期间禁止任何操作，请稍候…"}).encode())
            return
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
            elif cmd == "mode":
                ok, err = session.set_manual(bool(data.get("manual", False)))
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd == "free":
                ok, err = session.set_free(bool(data.get("on", False)))
                if not ok:
                    self._send(200, json.dumps({"ok": False, "error": err}).encode())
                    return
            elif cmd == "model_move":
                ok, err = session.model_move_cmd(data.get("from"), data.get("to"))
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
    print(f"[狼羊棋·web] 启动 (pid {os.getpid()})", flush=True)
    print(f"  监听  http://{args.host}:{args.port}", flush=True)
    print(f"  表库  {args.data_dir} (k ≤ {tb.max_k}，含开局局面)", flush=True)
    print(f"  引擎  tablebase 最优解", flush=True)
    print(f"  停止  Ctrl-C", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[狼羊棋·web] 已停止", flush=True)


if __name__ == "__main__":
    main()