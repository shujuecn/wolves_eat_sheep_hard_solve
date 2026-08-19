#!/usr/bin/env python3
"""ai_engine.py — 狼羊棋 DQN 模型推理助手（人机对战新版本使用）

由 C++ 程序（tools/human_vs_ai.cpp）以子进程方式常驻调用：
每行一个请求，行协议：
  MOVE <fen>    —— 对给定 FEN 局面，为"当前回合方"选出 greedy 最优走法（含战术过滤）
                  回应一行: "<from_idx> <to_idx>"（0..24 的格索引导）
  QUIT          —— 退出

启动成功后打印一行 READY，开始接受请求；任何错误打印一行 ERR <原因>。

只依赖 rules.py（纯标准库）+ torch，不依赖 pygame/PIL/matplotlib，
避免拖入 train_ai.py 的完整训练依赖链。
"""

import os
import sys
from pathlib import Path

# 必须在 import torch 之前设置：单线程推理，避免在共享机器上
# OpenMP 线程池创建失败（libgomp: Thread creation failed: EAGAIN）
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("KMP_BLOCKTIME", "0")

sys.path.insert(0, str(Path(__file__).parent))

import torch
from torch import nn

from rules import (
    BOARD_SIZE,
    DIRECTIONS,
    IDLE_LIMIT,
    MAX_MOVES,
    SHEEP,
    WOLF,
    GameState,
)

# ---------------- 与 train_ai.py 一致的常量与网络结构 ----------------
BOARD_CELLS = BOARD_SIZE * BOARD_SIZE
STATE_CHANNELS = 6
ACTION_COUNT = BOARD_CELLS * BOARD_CELLS


class QNetwork(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.layers = nn.Sequential(
            nn.Conv2d(STATE_CHANNELS, 64, kernel_size=3, padding=1),
            nn.ReLU(),
            nn.Conv2d(64, 64, kernel_size=3, padding=1),
            nn.ReLU(),
            nn.Flatten(),
            nn.Linear(64 * BOARD_CELLS, 256),
            nn.ReLU(),
        )
        self.wolf_head = nn.Linear(256, ACTION_COUNT)
        self.sheep_head = nn.Linear(256, ACTION_COUNT)

    def forward(self, state: torch.Tensor) -> torch.Tensor:
        features = self.layers(state)
        wolf_turn = state[:, 2, 0, 0].bool().unsqueeze(1)
        values = torch.empty(
            (state.shape[0], ACTION_COUNT),
            dtype=features.dtype,
            device=features.device,
        )
        wolf_rows = wolf_turn[:, 0]
        sheep_rows = ~wolf_rows
        if torch.any(wolf_rows):
            values[wolf_rows] = self.wolf_head(features[wolf_rows])
        if torch.any(sheep_rows):
            values[sheep_rows] = self.sheep_head(features[sheep_rows])
        return values


def encode_state(game: GameState) -> torch.Tensor:
    state = torch.zeros((STATE_CHANNELS, BOARD_SIZE, BOARD_SIZE), dtype=torch.float32)
    for row in range(BOARD_SIZE):
        for col in range(BOARD_SIZE):
            piece = game.board[row][col]
            if piece == WOLF:
                state[0, row, col] = 1.0
            elif piece == SHEEP:
                state[1, row, col] = 1.0
    state[2].fill_(1.0 if game.turn == WOLF else 0.0)
    state[3].fill_(game.move_count / MAX_MOVES)
    state[4].fill_(game.idle_streaks[WOLF] / IDLE_LIMIT)
    state[5].fill_(game.idle_streaks[SHEEP] / IDLE_LIMIT)
    return state


def encode_action(start: tuple[int, int], destination: tuple[int, int]) -> int:
    return (start[0] * BOARD_SIZE + start[1]) * BOARD_CELLS + (
        destination[0] * BOARD_SIZE + destination[1]
    )


def decode_action(action: int) -> tuple[tuple[int, int], tuple[int, int]]:
    start_index, destination_index = divmod(action, BOARD_CELLS)
    return (
        divmod(start_index, BOARD_SIZE),
        divmod(destination_index, BOARD_SIZE),
    )


def legal_actions(game: GameState) -> tuple[int, ...]:
    actions = []
    for row in range(BOARD_SIZE):
        for col in range(BOARD_SIZE):
            if game.board[row][col] != game.turn:
                continue
            for move in game.legal_moves_from((row, col)):
                actions.append(encode_action((row, col), move.destination))
    return tuple(actions)


# ---------------- 战术过滤（与 train_ai.py 一致，纯 Python） ----------------
def immediate_sheep_winning_actions(
    game: GameState,
    actions: tuple[int, ...] | None = None,
) -> tuple[int, ...]:
    if game.turn != SHEEP or game.winner is not None or game.wolf_liberties() > 3:
        return ()
    candidate_actions = actions if actions is not None else legal_actions(game)
    liberties = game.wolf_liberties()
    winning = []
    for action in candidate_actions:
        start, destination = decode_action(action)
        opened = sum(
            game.in_bounds(start[0] + row_step, start[1] + col_step)
            and game.board[start[0] + row_step][start[1] + col_step] == WOLF
            for row_step, col_step in DIRECTIONS
        )
        closed = sum(
            game.in_bounds(destination[0] + row_step, destination[1] + col_step)
            and game.board[destination[0] + row_step][destination[1] + col_step]
            == WOLF
            for row_step, col_step in DIRECTIONS
        )
        if liberties + opened - closed == 0:
            winning.append(action)
    return tuple(winning)


def partition_wolf_survival_actions(
    game: GameState,
    actions: tuple[int, ...] | None = None,
) -> tuple[tuple[int, ...], tuple[int, ...]]:
    import copy

    candidate_actions = actions if actions is not None else legal_actions(game)
    if game.turn != WOLF or game.winner is not None:
        return candidate_actions, ()

    safe = []
    losing = []
    for action in candidate_actions:
        start, destination = decode_action(action)
        trial = copy.deepcopy(game)
        if not trial.move(start, destination):
            continue
        sheep_wins_now = trial.winning_side == SHEEP
        sheep_wins_next = trial.winner is None and bool(
            immediate_sheep_winning_actions(trial)
        )
        if not sheep_wins_now and not sheep_wins_next:
            safe.append(action)
        else:
            losing.append(action)
    if not safe:
        return candidate_actions, ()
    return tuple(safe), tuple(losing)


def tactical_policy_actions(
    game: GameState,
    actions: tuple[int, ...] | None = None,
) -> tuple[int, ...]:
    candidate_actions = actions if actions is not None else legal_actions(game)
    if game.turn == SHEEP:
        return immediate_sheep_winning_actions(game, candidate_actions) or (
            candidate_actions
        )
    return partition_wolf_survival_actions(game, candidate_actions)[0]


def greedy_action(
    network: QNetwork,
    state: torch.Tensor,
    actions: tuple[int, ...],
    device: torch.device,
) -> int:
    with torch.no_grad():
        values = network(state.unsqueeze(0).to(device))[0]
        mask = torch.zeros(ACTION_COUNT, dtype=torch.bool, device=device)
        mask[list(actions)] = True
        return int(values.masked_fill(~mask, -torch.inf).argmax().item())


def choose_device() -> torch.device:
    if torch.backends.mps.is_available():
        return torch.device("mps")
    if torch.cuda.is_available():
        return torch.device("cuda")
    return torch.device("cpu")


def restore_network_state(network: QNetwork, model_state: dict, saved_channels: int) -> None:
    migrated_state = dict(model_state)
    legacy_head_weight = migrated_state.pop("layers.7.weight", None)
    legacy_head_bias = migrated_state.pop("layers.7.bias", None)
    if legacy_head_weight is not None and legacy_head_bias is not None:
        migrated_state["wolf_head.weight"] = legacy_head_weight
        migrated_state["wolf_head.bias"] = legacy_head_bias
        migrated_state["sheep_head.weight"] = legacy_head_weight.clone()
        migrated_state["sheep_head.bias"] = legacy_head_bias.clone()
    if saved_channels == STATE_CHANNELS:
        network.load_state_dict(migrated_state)
        return
    if saved_channels != 4:
        raise ValueError(f"unsupported checkpoint state channels: {saved_channels}")
    old_weight = migrated_state["layers.0.weight"]
    expanded_weight = torch.zeros(
        (old_weight.shape[0], STATE_CHANNELS, *old_weight.shape[2:]),
        dtype=old_weight.dtype,
        device=old_weight.device,
    )
    expanded_weight[:, :saved_channels] = old_weight
    migrated_state["layers.0.weight"] = expanded_weight
    network.load_state_dict(migrated_state)


# ---------------- FEN -> GameState（C++ to_fen 格式互逆） ----------------
def game_from_fen(fen: str) -> GameState:
    parts = fen.split()
    if len(parts) < 2:
        raise ValueError(f"bad fen: {fen!r}")
    rows = parts[0].split("/")
    if len(rows) != BOARD_SIZE:
        raise ValueError(f"bad fen rows: {fen!r}")
    game = GameState(idle_limit=None, max_moves=MAX_MOVES)
    game.board = [[None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)]
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
        if c != BOARD_SIZE:
            raise ValueError(f"bad fen row width in {fen!r}")
    game.turn = WOLF if parts[1] == "w" else SHEEP
    game.move_count = int(parts[2]) if len(parts) > 2 else 0
    game.winner = None
    game.idle_streaks = {WOLF: 0, SHEEP: 0}
    game.last_move_idle = False
    game._sync_piece_ids()
    return game


def main() -> int:
    checkpoint = "checkpoints/rv14/best_selfplay_dqn.pt"
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--checkpoint" and i + 1 < len(args):
            checkpoint = args[i + 1]
            i += 2
        else:
            i += 1
    base = Path(__file__).parent
    checkpoint = str(Path(checkpoint)) if Path(checkpoint).is_absolute() else str(base / checkpoint)

    out = sys.stdout
    err = sys.stderr
    try:
        if not Path(checkpoint).exists():
            raise FileNotFoundError(f"checkpoint not found: {checkpoint}")
        torch.set_num_threads(1)
        torch.set_num_interop_threads(1)
        device = choose_device()
        checkpoint_data = torch.load(checkpoint, map_location=device, weights_only=False)
        network = QNetwork().to(device)
        restore_network_state(
            network,
            checkpoint_data["model_state"],
            checkpoint_data.get("state_channels", 4),
        )
        network.eval()
        episodes = int(checkpoint_data.get("completed_episodes", 0))
        out.write(f"READY rv{checkpoint_data.get('config', {}).get('reward_version', '?')} episodes={episodes}\n")
        out.flush()
    except Exception as exc:  # noqa: BLE001
        err.write(f"[ai_engine] load failed: {exc}\n")
        err.flush()
        out.write(f"ERR load failed: {exc}\n")
        out.flush()
        return 1

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        cmd = parts[0]
        try:
            if cmd == "QUIT":
                break
            if cmd != "MOVE":
                out.write("ERR unknown command\n")
                out.flush()
                continue
            fen = " ".join(parts[1:])
            game = game_from_fen(fen)
            actions = legal_actions(game)
            if not actions:
                out.write("ERR no legal moves\n")
                out.flush()
                continue
            action = greedy_action(
                network,
                encode_state(game),
                tactical_policy_actions(game, actions),
                device,
            )
            start, destination = decode_action(action)
            from_idx = start[0] * BOARD_SIZE + start[1]
            to_idx = destination[0] * BOARD_SIZE + destination[1]
            out.write(f"{from_idx} {to_idx}\n")
            out.flush()
        except Exception as exc:  # noqa: BLE001
            err.write(f"[ai_engine] request failed: {exc}\n")
            err.flush()
            out.write(f"ERR {exc}\n")
            out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())