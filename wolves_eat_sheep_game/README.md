# 狼羊棋 · 规则库（rules.py）

5×5 狼羊棋的**纯标准库**规则实现（棋盘状态、走法生成、胜负判定），零第三方依赖。
这是当前仓库唯一保留的游戏代码模块：早期附带的 Pygame 终端游戏与 DQN 训练/推理
（`game.py` / `train_ai.py` / `ai_engine.py` 等）已全部移除；本规则库由
**Web 人机对战**（`web/server.py`）复用。

## 规则

- 初始时上三排为 15 只羊，第四排为空，第五排中间三格为狼；**狼先行**。
- 狼和羊每回合只能向上、下、左、右移动一格，不能斜走。
- 狼与羊之间恰好隔一个空格时，可直线跨过该空格，落到羊所在格并吃掉该羊；
  每回合最多吃一只，吃后立即换羊方。
- 羊只可移动，通过围堵取胜。
- **狼胜**：羊剩余不足 4 只。
- **羊胜**：3 只狼均无法移动。
- **和棋**：走闲规则（同一棋子连续往返 5 次）触发，或双方合计走满 150 步。

## 接口

- `GameState(idle_limit=None, max_moves=150)`：状态对象（`board` 5×5、
  `turn`、`winner`、`sheep_count`、`move_count` 等）。
- `GameState.move((r1,c1), (r2,c2)) -> bool`：走一步，返回是否合法（吃子自动结算）。
- `GameState.legal_moves_from((r,c)) -> list[Move]`：某格棋子的合法走法
  （`Move.destination`、`Move.captured`）。
- 常量：`WOLF / SHEEP / DRAW`。

## 使用

```python
import sys
sys.path.insert(0, "wolves_eat_sheep_game")
from rules import GameState, WOLF, SHEEP, DRAW

g = GameState(idle_limit=None, max_moves=150)
g.move((4, 1), (3, 1))          # 狼向上一步
print(g.winner, g.sheep_count)  # None 15
```

说明：棋盘上“狼跳过羊”的吃子规则见 `GameState.move` 的跳吃逻辑；完整的最优
走子（表库结论）不在此模块，见仓库根目录的逆向求解器与 `web/`。