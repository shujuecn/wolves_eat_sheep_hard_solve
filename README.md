# 狼羊棋 · 硬解（Wolves Eat Sheep — Hard Solve）

对 5×5 狼羊棋（3 狼 vs 15 羊，狼先行）做**完整逆向分析（retrograde analysis）**：
对 k=4…15 只羊的全部合法局面（含双方回合），求解每个局面的最优结果
**（狼胜 / 羊胜 / 和棋）**与**最短步数 DTM（distance-to-mate）**。

- 求解器：C++20，并行无锁原子，多线程
- 表库：约 **188 亿条目 / 17.5 GB**（k=4…15 全量已解出）
- 附带：浏览器人机对战（Web 版，表库 + DQN 混合引擎）

---

## 1. 问题定义

### 1.1 游戏规则

- 5×5 棋盘；初始 **15 只羊**占据最上 3 行，**3 只狼**占据最下行第 1/2/3 列；**狼先行**。
- **狼**：每步可直走一格，或**跳过相邻的羊**（跳吃，吃掉该羊，类似跳棋）。
- **羊**：每步直走一格（不能吃子）。
- **狼胜**：羊的数量被吃至 **< 4 只**。
- **羊胜**：三只狼全部**无合法移动**（被堵死）。
- **和棋**：双方同一局面重复达到 5 次（两点重复判定），或总步数 ≥ 150。

### 1.2 求解目标（Hard Solve）

对羊数 k = 4…15 的**每一个**合法局面（羊子组合随 k 变化）：

- 局面状态数 = 狼组合 `C(25,3)=2300` × 羊组合 `C(22,k)` × 回合(2)。
- 求解该局面在双方都最优的前提下的**结果**与**最短步数**，写入表库；
- k < 4 为平凡狼胜桶，不需要求解。

---

## 2. 解决方案

### 2.1 状态编码

- `wolf_rank`：三只狼在 25 格上的组合，共 2300 种（预计算移动表索引）。
- `sheep_subset`：k 只羊在剩余 22 格上的组合，`C(22,k)` 种。
- `turn`：狼回合 / 羊回合。
- 全表条目 = `2300 × C(22,k) × 2`，每条目 1 字节（结果 2bit + 距离 6bit，距离封顶 63）。

### 2.2 逆向分析（solver_retro）

1. **终局标定（init）**：直接终局（三狼全堵死 → 羊胜 d=0）、吃子后继
   （跳到 k-1 羊数，查已完成的 k-1 表得到跨桶结果）、必胜/必败初始距离。
2. **距离分层传播**：按最优距离 d=0…63 逐层处理队列；某状态的全部后继
   被解析完毕后（LOSS 计数归零）判负，距离取 `max(d, 跨桶最大 LOSS 距离 + 1)`；
   出现必胜后继则立即赋值并取最短距离。
3. 剩余 UNKNOWN 统一判为 **DRAW**。
4. 结果写入表库文件（`data/tb_test/wsf_tb_dtc_k*.bin`）。

另有早期**原始迭代求解器** `solve_all`（solver.cpp，逐步迭代逼近），保留作对照与校验。

### 2.3 表库文件格式

```
[64 字节头] WSTB | version | k | entry_bits | wolf_combos | sheep_combos | total_entries | completed(0/1) ...
[数据区]   每条目 1 字节：bits 0-1 结果(00 狼胜 / 01 羊胜 / 10 和棋 / 11 未知)，bits 2-7 最短步数(0..63)
```

- `completed=1` 表示整桶求解完成（求解中断的文件为半成品，启动时会自动重建重算）。
- 表库文件较大（最大约 3.2 GB/桶），**不入版本库**，由求解器生成。

---

## 3. 算法优化

- **移动表预计算**：所有 wolf_rank 的狼走法/跳吃、反向羊走法、邻接表全部预生成，
  传播期零分配、零重复计算。
- **距离桶 + 计数判负**：按 DTM 分层处理，避免每次传播都扫描全表；
  用「未决后继计数」在归零瞬间判定 LOSS，得到最优距离。
- **跨桶吃子一体化**：吃子后继直接引用已完成的 k-1 表；`loss_dists` 记录跨桶
  LOSS 最大距离，保证吃子路径的 DTM 也最优。
- **无锁并行**：`__atomic` 字节级原子操作（load/cas/fetch-sub）在多线程间安全更新
  表库与计数；按 64K 状态分块争抢任务，每线程独立局部距离桶，波间合并。
- **匿名内存 + 完成后一次性顺序写盘**：求解全程表库驻留匿名内存，零磁盘 I/O；
  整桶解完后一次性顺序写出（对 RAID5/机械盘友好）。此设计解决了一个实际瓶颈——
  原先 `mmap(MAP_SHARED)` 把 2.97GB 输出文件当工作内存全程随机写，脏页回写把
  32 个工作线程全部拖入内核写回节流的 `D` 状态（htop 里一片红 D、只剩 1 线程），
  修复后 k=12 从数小时爬行提速到 **977 秒**。

---

## 4. 结果

| k | 羊组合 C(22,k) | 条目数 | 表库大小 |
|---|---|---|---|
| 4 | 7,315 | 33,649,000 | 33 MB |
| 5 | 26,334 | 121,136,400 | 116 MB |
| 6 | 74,613 | 343,219,800 | 328 MB |
| 7 | 170,544 | 784,502,400 | 749 MB |
| 8 | 319,770 | 1,470,942,000 | 1.4 GB |
| 9 | 497,420 | 2,288,132,000 | 2.2 GB |
| 10 | 646,646 | 2,974,571,600 | 2.8 GB |
| 11 | 705,432 | 3,244,987,200 | 3.1 GB |
| 12 | 646,646 | 2,974,571,600 | 2.8 GB |
| 13 | 497,420 | 2,288,132,000 | 2.2 GB |
| 14 | 319,770 | 1,470,942,000 | 1.4 GB |
| 15 | 170,544 | 784,502,400 | 749 MB |
| **合计** | 4,082,454 | **18,779,288,400** | **17.5 GB** |

参考：k=12 经优化后在 32 线程下实测 **977 s** 完成（init 199 s + 传播 760 s + 收尾 2 s）；
全量 k=4…15 线性估算约 **1.7 小时**。

---

## 5. 使用方法

### 5.1 构建

需要 g++（C++20）。本机无系统 make 时使用仓库内自定义工具链：

```bash
export PATH=/home/agent074/tools/gcc-local/usr/bin:$PATH   # 仅自定义工具链环境需要
make -j8            # 构建全部求解/校验工具到 build/
```

或用 CMake：`cmake -B build-cmake && cmake --build build-cmake -j8`。

### 5.2 求解

```bash
# 逆向分析求解（推荐）：k=4→15 全量；支持 --start-k/--end-k 分段、断桶续算
./build/solve_retro --data-dir data/tb_test --threads 32 --start-k 4 --end-k 15

# 原始迭代求解器（对照用）
./build/solve_all --data-dir data/tb --verbose
```

说明：`data/tb_test` 里已解出的 `.bin` 表库不在版本库中（gitignore），
新克隆后需重新求解，或从原环境拷贝。

### 5.3 校验与统计

```bash
make test                                   # 规则引擎自检 + 走法对拍
./build/check_tb --data-dir data/tb_test --threads 32   # k=4/k=5 逐局面 minimax 独立性校验
./build/stat_tb  --data-dir data/tb_test                # 各桶结果分布统计（--raw 原始计数）
./build/verify   --data-dir data/tb_test --samples 10000
./build/dump_opening_book --data-dir data/tb_test --output opening_book.json
```

### 5.4 Web 游戏（人机对战）

```bash
pip3 install torch            # DQN 推理依赖（羊数大于表库最大 k 时使用）
python3 web/server.py --port 8080
```

本机访问（SSH 端口转发）：`ssh -L 8080:127.0.0.1:8080 <用户>@<服务器>` → 浏览器打开 `http://127.0.0.1:8080`。
引擎逻辑：羊数 ≤ 已解出最大 k（当前 15）用表库最优应手，否则用 DQN 模型
（`wolves_eat_sheep_game/checkpoints/rv14/best_selfplay_dqn.pt`）。
详见 [web/README.md](web/README.md)。

---

## 6. 目录结构

```
├── src/ include/          # C++ 求解核心（board/encode/symmetry/tablebase/solver/solver_retro）
├── tools/                 # 求解与校验工具入口
│   ├── solve_retro.cpp    # 逆向分析主求解器（唯一 CLI 参数 --data-dir/--threads/--start-k/--end-k）
│   ├── solve_all.cpp      # 原始迭代求解器
│   ├── check_tb.cpp       # 表库独立性校验（逐局面 minimax）
│   ├── stat_tb.cpp        # 表库结果分布统计
│   ├── verify.cpp         # 表库一致性验证
│   ├── crosscheck.cpp     # 走法生成对拍（vs Python rules.py）
│   ├── selfcheck.cpp      # 规则引擎自检
│   └── dump_opening_book.cpp  # 开局库导出
├── web/                   # 网页版人机对战（Python 标准库后端 + 单文件前端）
├── wolves_eat_sheep_game/ # 游戏规则库(rules.py) + DQN 推理(ai_engine.py) + 权重/素材
├── tests/crosscheck/      # 对拍脚本
├── data/tb_test/          # 已解表库输出（k=4..15，gitignore）
├── Makefile  CMakeLists.txt
└── hard_solve_fast.py     # Python 表库读取器（Web 引擎复用）
```

## 7. 工具一览

| 目标 / 命令 | 作用 |
|---|---|
| `make all` / `make` | 构建全部工具 |
| `make test` | 运行 selfcheck + crosscheck |
| `build/solve_retro` | 逆向分析求解器（主） |
| `build/check_tb` | 表库逐局面校验 |
| `build/stat_tb` | 表库分布统计 |
| `build/dump_opening_book` | 导出开局库 JSON |
| `make web` / `python3 web/server.py` | 网页人机对战 |