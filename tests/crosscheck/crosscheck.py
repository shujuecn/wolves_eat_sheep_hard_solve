#!/usr/bin/env python3
"""
crosscheck.py

与 C++ board 库做走法对拍：随机生成 N 个局面，
分别用 Python rules.py 和 C++ crosscheck 生成走法集，逐条比对。
"""

import json
import random
import subprocess
import sys
from pathlib import Path

# 将父目录加入 path 以导入 rules
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent.parent / "wolves_eat_sheep"))
from rules import BOARD_SIZE, SHEEP, WOLF, GameState, Move


def state_to_fen(game: GameState) -> str:
    """将 Python GameState 转为 FEN 字符串。"""
    parts = []
    for r in range(BOARD_SIZE):
        empty = 0
        row_str = ""
        for c in range(BOARD_SIZE):
            piece = game.board[r][c]
            if piece is None:
                empty += 1
            else:
                if empty > 0:
                    row_str += str(empty)
                    empty = 0
                row_str += "w" if piece == WOLF else "s"
        if empty > 0:
            row_str += str(empty)
        parts.append(row_str)
    fen = "/".join(parts)
    turn = "w" if game.turn == WOLF else "s"
    return f"{fen} {turn} {game.move_count}"


def fen_to_state(fen: str) -> GameState:
    """从 FEN 字符串恢复 GameState。"""
    game = GameState()
    parts = fen.split()
    board_part = parts[0]
    turn_char = parts[1] if len(parts) > 1 else "w"
    move_count = int(parts[2]) if len(parts) > 2 else 0

    # 清空棋盘
    game.board = [[None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)]
    rows = board_part.split("/")
    for r, row_str in enumerate(rows):
        c = 0
        for ch in row_str:
            if ch.isdigit():
                c += int(ch)
            else:
                game.board[r][c] = WOLF if ch == "w" else SHEEP
                c += 1
    game.turn = WOLF if turn_char == "w" else SHEEP
    game.move_count = move_count
    game._sync_piece_ids()
    game._update_winner()
    return game


def python_moves(game: GameState) -> list[dict]:
    """获取 Python 侧的合法走法列表（仅当前回合方，排序后）。"""
    moves = []
    for r in range(BOARD_SIZE):
        for c in range(BOARD_SIZE):
            piece = game.board[r][c]
            if piece is None or piece != game.turn:
                continue
            for move in game.legal_moves_from((r, c)):
                m = {
                    "from": [r, c],
                    "to": [move.destination[0], move.destination[1]],
                    "captured": (
                        [move.captured[0], move.captured[1]]
                        if move.captured
                        else None
                    ),
                }
                moves.append(m)
    # 排序以便比较
    moves.sort(key=lambda m: (m["from"][0], m["from"][1], m["to"][0], m["to"][1],
                              m["captured"][0] if m["captured"] else -1,
                              m["captured"][1] if m["captured"] else -1))
    return moves


def random_state(num_wolves: int = 3, num_sheep: int | None = None) -> GameState:
    """随机生成一个合法局面（棋子不重叠）。"""
    game = GameState()
    game.board = [[None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)]

    all_cells = [(r, c) for r in range(BOARD_SIZE) for c in range(BOARD_SIZE)]
    random.shuffle(all_cells)

    if num_sheep is None:
        num_sheep = random.randint(1, 15)

    # 放置狼
    for i in range(min(num_wolves, len(all_cells))):
        r, c = all_cells[i]
        game.board[r][c] = WOLF

    # 放置羊
    sheep_start = num_wolves
    sheep_end = sheep_start + num_sheep
    for i in range(sheep_start, min(sheep_end, len(all_cells))):
        r, c = all_cells[i]
        game.board[r][c] = SHEEP

    game.turn = random.choice([WOLF, SHEEP])
    game.move_count = random.randint(0, 149)
    game._sync_piece_ids()
    return game


def run_cpp_crosscheck(cpp_binary: Path, fens: list[str]) -> dict[str, dict]:
    """用 C++ crosscheck 处理一批 FEN，返回 {fen: result}。"""
    input_text = "\n".join(fens) + "\n"
    result = subprocess.run(
        [str(cpp_binary)],
        input=input_text,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        print(f"[ERROR] C++ crosscheck failed: {result.stderr}")
        return {}

    output = {}
    for line in result.stdout.strip().split("\n"):
        if not line:
            continue
        try:
            obj = json.loads(line)
            fen = obj["fen"]
            output[fen] = obj
        except json.JSONDecodeError as e:
            print(f"[WARN] JSON parse error: {e} | line: {line[:100]}")
    return output


def compare_moves(py_moves: list[dict], cpp_moves: list[dict]) -> list[str]:
    """比较两边的走法列表，返回差异信息列表。"""
    diffs = []
    if len(py_moves) != len(cpp_moves):
        diffs.append(f"count mismatch: Python={len(py_moves)}, C++={len(cpp_moves)}")

    # 逐条对比
    for i in range(max(len(py_moves), len(cpp_moves))):
        pm = py_moves[i] if i < len(py_moves) else None
        cm = cpp_moves[i] if i < len(cpp_moves) else None
        if pm != cm:
            diffs.append(f"  move[{i}]: Python={pm} vs C++={cm}")
    return diffs


def main():
    # 查找 C++ 二进制
    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir.parent.parent  # hard_solve 根目录
    build_dir = project_dir / "build"
    cpp_binary = build_dir / "crosscheck"

    if not cpp_binary.exists():
        print(f"[ERROR] C++ binary not found: {cpp_binary}")
        print("Run: cmake -B build && cmake --build build")
        sys.exit(1)

    NUM_CASES = 50000
    print(f"Generating {NUM_CASES} random states...")

    random.seed(42)

    # 批量生成随机局面
    states: list[GameState] = []
    fens: list[str] = []

    # 加入初始局面
    initial = GameState()
    initial._update_winner()
    states.append(initial)
    fens.append(state_to_fen(initial))

    # 加入已知边界局面
    per_k = max(50, NUM_CASES // 15)
    for k in range(1, 16):
        for _ in range(per_k):
            s = random_state(num_sheep=k)
            s._update_winner()
            states.append(s)
            fens.append(state_to_fen(s))

    print(f"Total states: {len(fens)} (including initial + edge cases)")

    # 跑 C++ 对拍
    print("Running C++ crosscheck...")
    cpp_results = run_cpp_crosscheck(cpp_binary, fens)

    if not cpp_results:
        print("[FATAL] No results from C++ crosscheck")
        sys.exit(1)

    # 逐条比对
    mismatches = 0
    for i, (state, fen) in enumerate(zip(states, fens)):
        py_moves = python_moves(state)

        cpp_obj = cpp_results.get(fen)
        if cpp_obj is None:
            print(f"[MISS] FEN not in C++ output: {fen}")
            mismatches += 1
            continue

        if "error" in cpp_obj:
            print(f"[ERROR] C++ parse error for: {fen} -> {cpp_obj['error']}")
            mismatches += 1
            continue

        cpp_moves = cpp_obj.get("moves", [])

        diffs = compare_moves(py_moves, cpp_moves)
        if diffs:
            mismatches += 1
            print(f"\n[MISMATCH #{mismatches}] fen={fen}")
            print(f"  State: turn={state.turn}, sheep={state.sheep_count}, "
                  f"wolves={sum(1 for r in range(5) for c in range(5) if state.board[r][c]==WOLF)}")
            for d in diffs:
                print(d)

        # 也检查终局判定
        py_terminal = state.winner
        cpp_terminal = cpp_obj.get("terminal")
        # 映射 Python winner 到终端类型
        if py_terminal == WOLF:
            py_term_type = "wolf_win"
        elif py_terminal == "sheep_blocked":
            py_term_type = "sheep_win"
        elif py_terminal == "draw":
            py_term_type = "draw"
        else:
            py_term_type = None

        if py_term_type != cpp_terminal:
            mismatches += 1
            print(f"\n[TERMINAL MISMATCH #{mismatches}] fen={fen}")
            print(f"  Python: {py_terminal} -> {py_term_type}")
            print(f"  C++:    {cpp_terminal}")

        if i % 1000 == 0 and i > 0:
            print(f"  ... checked {i}/{len(fens)}, mismatches={mismatches}")

    print(f"\n=== RESULT: {mismatches} mismatches out of {len(fens)} states ===")
    if mismatches == 0:
        print("✅ ALL PASS — C++ board matches Python rules.py exactly!")
    else:
        print(f"❌ {mismatches} MISMATCHES FOUND")
        sys.exit(1)


if __name__ == "__main__":
    main()