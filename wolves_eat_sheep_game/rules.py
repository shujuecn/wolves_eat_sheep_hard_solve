from collections import deque
from dataclasses import dataclass


BOARD_SIZE = 5
MAX_MOVES = 150
RULES_VERSION = 2
SHEEP = "sheep"
WOLF = "wolf"
DRAW = "draw"
SHEEP_BLOCKED = "sheep_blocked"
WOLF_IDLE_LOSS = "wolf_idle_loss"
# Kept so older replay files can still be decoded; new games never emit it.
SHEEP_IDLE_LOSS = "sheep_idle_loss"
IDLE_LIMIT = 5
IDLE_RULE = "two_point_repetition"
DIRECTIONS = ((-1, 0), (1, 0), (0, -1), (0, 1))


@dataclass(frozen=True)
class Move:
    destination: tuple[int, int]
    captured: tuple[int, int] | None = None


class GameState:
    def __init__(
        self,
        idle_limit: int | None = IDLE_LIMIT,
        max_moves: int = MAX_MOVES,
    ) -> None:
        self.idle_limit = idle_limit
        self.max_moves = max_moves
        self.reset()

    def reset(self) -> None:
        self.board = [[None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)]
        for row in range(3):
            for col in range(BOARD_SIZE):
                self.board[row][col] = SHEEP
        for col in (1, 2, 3):
            self.board[4][col] = WOLF

        self.turn = WOLF
        self.move_count = 0
        self.winner: str | None = None
        self.idle_streaks = {WOLF: 0, SHEEP: 0}
        self.last_move_idle = False
        self.piece_ids: dict[tuple[int, int], str] = {}
        self.piece_histories: dict[str, deque] = {}
        self.last_piece_by_side = {WOLF: None, SHEEP: None}
        self._sync_piece_ids()

    @staticmethod
    def in_bounds(row: int, col: int) -> bool:
        return 0 <= row < BOARD_SIZE and 0 <= col < BOARD_SIZE

    @property
    def sheep_count(self) -> int:
        return sum(piece == SHEEP for row in self.board for piece in row)

    @property
    def winning_side(self) -> str | None:
        if self.winner in (WOLF, SHEEP_IDLE_LOSS):
            return WOLF
        if self.winner in (SHEEP_BLOCKED, WOLF_IDLE_LOSS):
            return SHEEP
        return None

    def legal_moves_from(self, position: tuple[int, int]) -> list[Move]:
        row, col = position
        piece = self.board[row][col]
        if piece is None:
            return []

        moves: list[Move] = []
        for row_step, col_step in DIRECTIONS:
            next_row = row + row_step
            next_col = col + col_step
            if not self.in_bounds(next_row, next_col):
                continue

            if self.board[next_row][next_col] is not None:
                continue

            moves.append(Move((next_row, next_col)))
            prey_row = row + 2 * row_step
            prey_col = col + 2 * col_step
            if (
                piece == WOLF
                and self.in_bounds(prey_row, prey_col)
                and self.board[prey_row][prey_col] == SHEEP
            ):
                moves.append(Move((prey_row, prey_col), (prey_row, prey_col)))
        return moves

    def wolf_capture_targets(self) -> set[tuple[int, int]]:
        return {
            move.captured
            for row in range(BOARD_SIZE)
            for col in range(BOARD_SIZE)
            if self.board[row][col] == WOLF
            for move in self.legal_moves_from((row, col))
            if move.captured is not None
        }

    def wolf_mobility(self) -> int:
        return sum(
            len(self.legal_moves_from((row, col)))
            for row in range(BOARD_SIZE)
            for col in range(BOARD_SIZE)
            if self.board[row][col] == WOLF
        )

    def wolf_liberties(self) -> int:
        return sum(
            self.in_bounds(row + row_step, col + col_step)
            and self.board[row + row_step][col + col_step] is None
            for row in range(BOARD_SIZE)
            for col in range(BOARD_SIZE)
            if self.board[row][col] == WOLF
            for row_step, col_step in DIRECTIONS
        )

    def _sync_piece_ids(self) -> None:
        occupied = {
            (row, col): self.board[row][col]
            for row in range(BOARD_SIZE)
            for col in range(BOARD_SIZE)
            if self.board[row][col] is not None
        }
        if {
            position: self.board[position[0]][position[1]]
            for position in self.piece_ids
        } == occupied:
            return

        self.piece_ids = {}
        self.piece_histories = {}
        self.last_piece_by_side = {WOLF: None, SHEEP: None}
        for position, piece in occupied.items():
            piece_id = f"{piece}:{position[0]}:{position[1]}"
            self.piece_ids[position] = piece_id
            self.piece_histories[piece_id] = deque(
                (position,), maxlen=IDLE_LIMIT + 2
            )

    def _back_and_forth_count(self, history: deque) -> int:
        positions = list(history)
        if len(positions) < 3:
            return 0

        count = 1
        for index in range(len(positions) - 3, -1, -1):
            if positions[index] != positions[index + 2]:
                break
            count += 1
        return count if count >= 2 else 0

    def move(self, start: tuple[int, int], destination: tuple[int, int]) -> bool:
        if self.winner or self.board[start[0]][start[1]] != self.turn:
            return False

        move = next(
            (
                candidate
                for candidate in self.legal_moves_from(start)
                if candidate.destination == destination
            ),
            None,
        )
        if move is None:
            return False

        self._sync_piece_ids()
        piece = self.board[start[0]][start[1]]
        piece_id = self.piece_ids.pop(start)
        history = self.piece_histories[piece_id]
        if self.last_piece_by_side[piece] != piece_id:
            history.clear()
            history.append(start)
        self.board[start[0]][start[1]] = None
        if move.captured:
            self.board[move.captured[0]][move.captured[1]] = None
            captured_id = self.piece_ids.pop(move.captured, None)
            if captured_id:
                self.piece_histories.pop(captured_id, None)
        self.board[destination[0]][destination[1]] = piece
        self.piece_ids[destination] = piece_id
        history.append(destination)
        self.last_piece_by_side[piece] = piece_id

        repeat_count = self._back_and_forth_count(history)
        self.last_move_idle = repeat_count > 0
        self.idle_streaks[piece] = repeat_count

        self.move_count += 1
        self._update_winner()
        if (
            self.winner is None
            and self.idle_limit is not None
            and all(
                self.idle_streaks[side] >= self.idle_limit
                for side in (WOLF, SHEEP)
            )
        ):
            self.winner = DRAW
        if self.winner is None and self.move_count >= self.max_moves:
            self.winner = DRAW
        if self.winner is None:
            self.turn = SHEEP if self.turn == WOLF else WOLF
        return True

    def _update_winner(self) -> None:
        if self.sheep_count < 4:
            self.winner = WOLF
            return

        wolves_can_move = any(
            self.legal_moves_from((row, col))
            for row in range(BOARD_SIZE)
            for col in range(BOARD_SIZE)
            if self.board[row][col] == WOLF
        )
        if not wolves_can_move:
            self.winner = SHEEP_BLOCKED
