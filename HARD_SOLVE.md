# 狼羊棋硬解工程方案（C++）

> 版本：v1.1（实现版，2026-08-17）
> 目标：用 C++ 复刻狼羊棋规则，基于**逆向归纳（retrograde analysis）**对该游戏做**完全数学求解**（strong solving），输出表库与开局库。
> 本版依据仓库当前实际代码（`src/`、`include/`、`tools/`）与真实求解日志更新，不再是草案。

---

## 当前求解进度（截至 2026-08-18，逆推求解器终版验证）

> 自 2026-08-18 起，求解改用 `optimize/retrograde` 分支的**逆推求解器**（`src/solver_retro.cpp`，见 PROGRESS_RETROGRADE.md），
> 结果码与原表库 100% 一致，且**距离（DTM）全部最优**（原价值迭代表距离非最优，见 §8.2）。

| k | 条目数（含轮次） | 文件大小 | 状态 | 逆推耗时 | 结果码 vs 原表库 | check_tb 距离 |
|---|---|---|---|---|---|---|
| 4 | 33,649,000 | 32 MB | ✅ 已完成 | 5s（80线程） | 0 差异 | OPTIMAL |
| 5 | 121,136,400 | 116 MB | ✅ 已完成 | 23s（60线程） | 0 差异 | OPTIMAL |
| 6 | 343,219,864 | 327 MB | ✅ 已完成 | 103s（60线程） | 0 差异 | OPTIMAL |
| 7 | 784,502,464 | 748 MB | ✅ 已完成 | 420s（30线程） | 0 差异 | OPTIMAL |
| 8 | 1,470,942,064 | 1.37 GB | ✅ 已完成 | 501s（28线程） | 无原表可比 | OPTIMAL |
| 9–15 | 见 §9 | - | 待解 | - | - | - |

- 输出目录 `data/tb_test/`；k=1、2、3 是**平凡狼胜桶**（羊 < 4 直接狼胜），不生成表库文件。
- 全部结果经 `tools/compare_tb`（vs 原表库）与 `tools/check_tb`（独立 minimax 重算）双重复核。

---

## 0. 与原方案的关键差异（实现现状）

原 v1.0 草案里若干“计划项”在落地时做了取舍，以下以**当前代码为准**：

1. **没有左右镜像压缩**。`use_symmetry` 配置项存在且在启动时打印 `on`，但 `solver.cpp` 求解路径**完全不使用它**，表库按全量状态线性存储（见 §4）。镜像对称仅用于 `verify` 工具做抽查自检。
2. **不是“两阶段”（先狼视角再羊视角）**，而是**逐桶统一逆推**：每个桶内狼回合/羊回合两个半桶一起做不动点迭代（见 §2）。
3. **不是 worklist BFS**，而是**并行价值迭代（Jacobi 风格）**：反复全表扫描直到无新状态被解出，或达到 `max_iters` 上限。
4. **条目是 1 字节（8 bit）**：`result` 2 bit + `distance` 6 bit（0–63），不是草案的“推荐 10 bit”。
5. **每个 k 一个文件**（`wsf_tb_dtc_kNN.bin`），狼/羊两个半桶交织在同一文件内，不是 `_w.bin` / `_s.bin` 两个文件。
6. **表库状态不含 `move_count`**：150 步规则不进入表库，`DRAW` 只表示“双方都无法强制获胜”（循环/拖平），150 步截断仅在查表/开局校验层用 `board.h::is_terminal` 处理（见 §1）。

---

## 1. 游戏规则形式化（与代码 `board.cpp` 一致）

| 项目 | 定义 |
|---|---|
| 棋盘 | 5×5，行 0–4 自上而下，列 0–4 自左而右；格子索引 `idx = row*5+col`（0–24） |
| 初始局面 | 行 0–2 共 15 只羊；行 4 的 `(4,1)(4,2)(4,3)` 3 只狼；狼先手 |
| 移动 | 狼/羊每回合沿上下左右移动一格，不可斜走、不可走入已占格 |
| 吃子 | 仅狼能吃：狼与羊**恰好隔一个空格**时，狼跨过空格落到羊格并吃掉；每回合至多吃 1 只，吃后立即换羊方 |
| 狼胜 | 羊剩余 < 4（即 ≤ 3） |
| 羊胜 | 3 只狼全部无合法移动（被围死） |
| 平局 | 走满 150 步（规则层）；表库层 `DRAW` = 无强制胜（循环） |
| 棋子 | 同侧棋子不可区分（3 狼、k 羊各视为同种） |

**规则层终局判定**（`board.h::is_terminal`，含 `move_count`）：

```text
sheep_count < 4          → WOLF_WIN
三狼均无合法移动          → SHEEP_WIN
move_count >= 150        → DRAW
否则                      → UNKNOWN
```

**表库层终局判定**（`solver.cpp::check_terminal`，不含 `move_count`）：

```text
k < 4（羊不足 4）         → WOLF_WIN（平凡桶，不落盘）
三狼均无合法移动          → SHEEP_WIN（distance 0）
```

表库求解**不携带步数**，因此表库的 `DRAW` 语义是“双方都没有强制胜（存在循环路径）”，不是“150 步耗尽”。开局库导出时若强制胜距离超出 150 步口径再单独处理（见 §2 说明）。

**走闲规则**：与 v1.0 结论一致——在双方最优假设下走闲不推进，可忽略；输方的拖平手段被 150 步截断覆盖，表库按“无步数上限的博弈论值”求解即可。

---

## 2. 实际算法：逐桶并行价值迭代

### 2.1 依赖关系（无环）

吃子是唯一改变羊数的动作，且每次至多吃 1 只（k → k−1），因此：

- k 桶的狼吃子后继落在 **k−1 桶**（必须已解）；
- k 桶的狼普通移动、羊移动的后继**仍在 k 桶内**（桶内迭代处理）。

所以按 **k = 4 → 15 自底向上**逐个求解，依赖天然无环。k=4 是第一个真实桶，其“狼一步吃即进入 k=3（平凡狼胜）”作为逆推种子。

### 2.2 单桶求解流程（`solver.cpp::solve_bucket`）

```text
solve_bucket(k):
    确保 k−1 桶已加载（k>4 时）
    创建/打开 k 桶文件，全部初始化为 UNKNOWN（fill_unknown）
    标记终局（mark_terminals）：
        对每个 (wolf_rank, sheep_rank, turn)，
        若 k<4 → WOLF_WIN(0)；若三狼全被堵 → SHEEP_WIN(0)
    迭代直到无变化（或达到 max_iters）：
        for 每个 wolf_rank 块（并行）:
            for 每个 sheep_rank:
                狼回合状态 evaluate_state(...)
                羊回合状态 evaluate_state(...)
    剩余 UNKNOWN → DRAW(0)
    写 completed 标记
```

### 2.3 状态评估（`evaluate_state`，狼/羊对称）

```text
evaluate_state(s):
    if s 已解（非 UNKNOWN）: return   # “只解一次，不回改”
    for 每个后继 t:
        取 t 的值 result_t、距离 dist_t
        若 t 是“我方胜”  → found_win = true；best = min(best, dist_t+1)
        若 t 非“对方胜”  → all_opp_win = false
        否则             → max_opp = max(max_opp, dist_t)
    if found_win:                   s = 我方胜(best)     # ∃ 致胜走法
    elif 有走法 且 all_opp_win:      s = 对方胜(max_opp+1) # 全部走法都送对方赢
    else:                           s = 保持 UNKNOWN     # 平/未定，继续迭代
```

- 狼回合的“我方胜”= `WOLF_WIN`，“对方胜”= `SHEEP_WIN`；羊回合相反。
- 距离 = **单步（ply）数**：胜 = `1 + min(致胜后继距离)`；负 = `1 + max(全部后继距离)`；终局/平局 = 0。
- 距离累计是 ply 口径；150 步若按“双方合计 150 步 = 300 ply”计，则查表层需把 ply 距离换算后与 150 步口径对齐（见 §1 说明）。

### 2.4 结果码

| 值 | 含义 |
|---|---|
| 0 | `WOLF_WIN`（羊 < 4） |
| 1 | `SHEEP_WIN`（三狼被围死） |
| 2 | `DRAW`（无强制胜） |
| 3 | `UNKNOWN`（未定，仅求解中/未完成桶） |

---

## 3. 状态编码与表库格式（实际）

### 3.1 桶大小与线性索引

- 狼：3 只在 25 格中的无序组合 → `C(25,3) = 2300`（`wolf_rank ∈ [0,2300)`）。
- 羊：k 只在剩余 22 格中的无序组合 → `C(22,k)`（`sheep_rank ∈ [0,C(22,k))`）。
- 轮次：1 bit（`turn`：`0=狼回合`，`1=羊回合`）。

```text
idx = wolf_rank * C(22,k) * 2 + sheep_rank * 2 + turn
bucket_size(k) = 2300 * C(22,k) * 2      # 条目数（每条目 1 字节）
```

编码为**组合序（combinatorial number system / Lehmer 逆）**，`encode.cpp` 里用预计算的二项式系数 `BINOM[n][k]` 实现；`WOLF_INFO[wolf_rank]` 预存每个狼组合的 3 个位置、22 个空闲格 `free_list` 与坐标映射。

### 3.2 条目编码（1 字节）

```text
bits 0-1 = result      （0/1/2/3）
bits 2-7 = distance    （0-63，6 bit）
entry = (result & 0x03) | ((distance << 2) & 0xFC)
```

> 距离字段为 6 bit（0–63）。当前 k≤6 观测到最大距离接近 63，属临界但未溢出；若后续桶出现 >63 的距离，需要扩位（见 §8 风险）。

### 3.3 文件布局与 64 字节头

```
data/tb/
├── wsf_tb_dtc_k04.bin     # k=4 桶（狼/羊两半桶交织）
├── wsf_tb_dtc_k05.bin
├── ...
└── wsf_tb_dtc_k15.bin
```

`TBHeader`（64 字节，`tablebase.h`）：

```text
offset 0    char magic[4]       = "WSTB"
offset 4    uint8 version       = 规则版本
offset 5    uint8 k             = 羊数
offset 6    uint8 entry_bits    = 8
offset 8    uint32 wolf_combos  = 2300
offset 12   uint32 sheep_combos = C(22,k)
offset 16   uint64 total_entries
offset 24   uint8 completed     = 0/1   ← 断点续算判断位
offset 25   reserved（填充到 64）
```

文件总大小 = `64 + bucket_size(k)` 字节；`mmap` 映射后随机访问 O(1)。`completed` 字节（offset 24）=1 表示该桶已完成，`run_solve.sh` 据此跳过。

---

## 4. 对称性：未压缩（仅校验）

- 对称群仍只有**左右镜像** `{e, M}` 一个合法操作（分析同 v1.0 §3.1）。
- **当前求解器未做镜像规范化/压缩**，表库存全量状态。因此文件大小是“未减半”的口径（§9）。
- `symmetry.cpp` 提供 `canonical_ranks` / 镜像工具，仅被 `verify.cpp` 用来抽查 `value(s) == value(M(s))`，不参与求解压缩。

---

## 5. 并行与参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--threads` | `nproc`（本机 80） | 工作线程数 |
| `--block-size` | 100 | 每个并行块包含的 wolf_rank 数 → `2300/100 = 23` 块 |
| `--max-iters` | 200 | 每桶迭代次数安全上限 |
| `--start-k` / `--end-k` | 4 / 15 | 求解羊数桶范围 |
| `--data-dir` | `./data/tb` | 表库目录 |

并行粒度：按 `wolf_rank` 分块（`process_block`），每块内顺序扫 `sheep_rank × turn`；多个块用 `std::thread` 并发。块间在每次迭代末尾 `join`，因此同一桶内不同线程会读写共享表，属于**并行价值迭代**（结果单调收敛，但见 §8 距离问题）。

---

## 6. 工程结构与构建

### 6.1 目录与文件

```
├── CMakeLists.txt            # CMake 构建（备选）
├── Makefile                  # 实际使用的构建（run_solve.sh 调用）
├── run_solve.sh              # 一键求解脚本
├── include/
│   ├── board.h / encode.h / tablebase.h / solver.h / symmetry.h
├── src/
│   ├── board.cpp   encode.cpp   tablebase.cpp   solver.cpp   symmetry.cpp
├── tools/
│   ├── solve_all.cpp          # 主求解入口
│   ├── dump_opening_book.cpp  # 导出开局库 JSON
│   ├── verify.cpp             # 随机镜像 + 终局一致性抽查
│   ├── selfcheck.cpp          # 走法/终局/FEN 自检
│   ├── crosscheck.cpp         # FEN → 走法 JSON（供 Python 对拍）
│   └── check_tb.cpp           # 逐局面 minimax 一致性独立校验（新增）
└── data/tb/                   # 表库输出
```

### 6.2 构建

编译器用本地从 `.deb` 提取的 g++（`/tmp/gcc-local`），`run_solve.sh` 会先导出 `PATH` / `LD_LIBRARY_PATH`。

```bash
export PATH="/tmp/gcc-local/usr/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/gcc-local/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
make -j80 all              # 构建全部工具
```

- 编译标准 C++20，优化 `-O3 -march=native -flto`，`-pthread`（OpenMP 可选，CMake 探测）。
- `make` 目标：`all`、`solve`、`verify`、`check-tb`、`dump-book`、`test`、`clean`。

---

## 7. 执行方式（`run_solve.sh`）

```bash
./run_solve.sh                     # 全量 k=4→15
./run_solve.sh --dry-run           # 只打印规模预算
./run_solve.sh --start-k 7         # 从 k=7 开始（跳过已完成桶）
./run_solve.sh --resume            # 断点续算（默认就会跳过 completed=1 的桶）
```

脚本主流程：

1. 环境检查（g++/cmake/make、CPU/内存/磁盘）；
2. 打印规模预算表；
3. `make -j all` 编译；
4. **逐桶求解**：对 k=4..15，已完成则跳过，否则删除半成品文件并调用 `solve_all --start-k k --end-k k ...`；
5. 导出开局库 `opening_book.json`；
6. `verify` 抽查（默认 10000 样本）；
7. 输出最终统计。

日志写入 `solve_log_YYYYMMDD_HHMMSS.log`（UTF-8 纯文本），摘要写入 `solve_summary.txt`。每个桶完成后写 `completed=1`，天然支持断点续算。

---

## 8. 验证与已知问题

### 8.1 已有验证工具

| 工具 | 作用 |
|---|---|
| `selfcheck` | `board.cpp` 走法生成 / 终局判定 / FEN 的单元自检 |
| `crosscheck` | 读 FEN，输出走法 JSON，供与 Python `rules.py` 对拍 |
| `verify` | 随机局面镜像对称 + 终局一致性抽查（默认 10000 样本） |
| `check_tb` | **逐局面**独立 minimax 一致性校验（不依赖 solver 逻辑，只读） |

`check_tb` 结果（逆推求解器输出，`data/tb_test/`）：

| 桶 | 结果校验 | 距离校验 |
|---|---|---|
| k=4 | ✅ PASS（0 错误，0 UNKNOWN） | ✅ OPTIMAL（0 状态偏） |
| k=5 | ✅ PASS（0 错误，0 UNKNOWN） | ✅ OPTIMAL（0 状态偏） |
| k=6 | ✅ PASS（0 错误，0 UNKNOWN） | ✅ OPTIMAL（0 状态偏） |
| k=7 | ✅ PASS（0 错误，0 UNKNOWN） | ✅ OPTIMAL（0 状态偏） |
| k=8 | ✅ PASS（0 错误，0 UNKNOWN） | ✅ OPTIMAL（0 状态偏） |

原价值迭代表（`data/tb/`）的 `check_tb` 对照：k=4 距离次优 378,169 状态（1.1%）、k=5 次优 9,979,157（8.2%），且 k≥6 原表距离同样非最优（并含少量可疑 `stored=0/0` 终端标注）。

### 8.2 原价值迭代的已知问题：距离（DTM）非最优 —— 已被逆推求解器解决

**原求解器**的胜负/和结果 100% 正确（`check_tb` 逐局面复核 0 结果错误），但**距离字段系统性偏大**。

根因：`evaluate_state` 采用“已解（非 UNKNOWN）就跳过、不回改” + 并行价值迭代。价值迭代不是严格按距离分层，某个局面第一次被解出时，其最优后继可能尚未就绪，只能用次优后继的距离；之后不再回改，导致 DTM 被抬高。

**解决方案（已落地）**：`optimize/retrograde` 分支的逆推求解器按 64 个距离桶分层传播，天然保证距离单调正确；传播阶段支持“胜线升级”（init 继承距离可被更短线覆盖）与“跨桶吃子 max_loss 并入”，最终全桶经 `check_tb` 验证 **DTM 最优**。CPU 耗时还从价值迭代的 101s/610s/3,134s（k=4/5/6）降至 5s/23s/103s。

### 8.3 其它风险

| 风险 | 说明 | 对策 |
|---|---|---|
| 距离 6 bit（0–63）上限 | 若某桶出现 >63 的距离会回绕 | 扩到 8 bit（改 `tb_pack`/`tb_distance` 与条目位宽） |
| 未做可达性过滤 | k 大的桶大量羊组合从初始局面不可达，浪费算力/磁盘 | 二期正向 BFS 可达集压缩（可选） |
| 全量未镜像 | 磁盘/内存约 17.5 GB（见 §9） | 如需压缩，再启用镜像规范化 |

---

## 9. 规模与耗时（实际口径，未镜像）

全量状态（k=1..15，含轮次）= `2300 × Σ_{k=1..15} C(22,k) × 2`：

```text
Σ_{k=1..15} C(22,k)  = 4,084,247
无轮次局面            = 2300 × 4,084,247   = 9,393,768,100
含轮次局面            = × 2                = 18,787,536,200   （约 187.9 亿）
落盘桶 k=4..15        = 18,779,288,400 条目
存储（1 字节/条目）    ≈ 17.5 GB
```

各桶条目数（= 文件字节数，误差 64 字节头）：

```text
k= 4:    33,649,000     k= 9: 2,288,132,000
k= 5:   121,136,400     k=10: 2,974,571,600
k= 6:   343,219,800     k=11: 3,244,987,200   ← 最大桶
k= 7:   784,502,400     k=12: 2,974,571,600
k= 8: 1,470,942,000     k=13: 2,288,132,000
                        k=14: 1,470,942,000
                        k=15:   784,502,400
```

实测耗时（80 线程）：k=4 约 101s、k=5 约 610s、k=6 约 3,134s（52m14s）。k 越大单桶越慢（k=6 是 k=5 的约 2.8 倍局面数、耗时约 5 倍），全量 k=4→15 预计在**数小时级**（与机器负载强相关）。

---

## 10. 后续计划

1. ✅ **DTM 距离修复**：已由逆推求解器（`src/solver_retro.cpp`）完成，k=4..8 全部 OPTIMAL。
2. **继续 k=9 → 15 求解**（`./build/solve_retro --data-dir data/tb_test --threads 60 --start-k 9 --end-k 15`；k=9 约 2.3G 条目，预计 ~15–25 分钟/桶，全量数小时内）。
3. **导出开局库** `opening_book.json`（初始 k=15 局面 → 结果/距离/最优首手），并校验 150 步口径。
4. **交叉验证**：`crosscheck` 与 Python `rules.py` 走法集对拍；镜像对称全量自检。
5. **可选优化**：镜像压缩、可达集过滤、距离字段扩位。

---

## 附录 A：关键源码入口

- 求解核心：`src/solver.cpp`（`solve_bucket` / `evaluate_state` / `process_block`）
- 编码：`src/encode.cpp`（`bucket_size` / `state_index` / 组合编码）
- 表库：`src/tablebase.cpp` + `include/tablebase.h`（`TBHeader` / `tb_pack`）
- 走法/终局：`src/board.cpp`（`gen_moves` / `apply` / `is_terminal`）
- 主入口：`tools/solve_all.cpp`；一键脚本：`run_solve.sh`
- 独立校验：`tools/check_tb.cpp`

---

## 11. 终端工具：复刻游戏与表库解棋（2024-08 新增）

> **玩家向的启动/玩法教程见 [`docs/PLAY_GUIDE.md`](docs/PLAY_GUIDE.md)**（构建、双人操作、规则速查、示例对局、解棋器用法、FAQ）。本节是面向开发者的工程说明。

两个新工具的定位与用法（不影响正在运行的求解进程：均为**单线程 + 只读 mmap**，
只加载「文件大小 == 64 + bucket_size(k)」且头部 `completed==1` 的完成桶，
求解程序写桶时文件不落盘，因此绝无读写冲突）：

### 11.1 `./build/play` —— C++ 复刻 Python GUI 的双人对局（终端版）

- 规则与 `wolves_eat_sheep_game/rules.py` 完全一致：狼先行、正交移动、
  狼跳吃（隔空格吃两格外的羊）、羊<4 狼胜、三狼被堵羊胜、
  **150 步平局**、**双方同一棋子连续往返 ≥5 次判平**（闲步规则已移植）。
- 输入：`a1..e5` 坐标（列 a-e × 行 1-5）。单格=选中棋子（合法落点金色高亮），
  再输目标格落子；也可一次两格 `b3 c3` 直接走子。
- 命令：`u` 悔棋，`r` 旋转 180°（与 Python GUI 的旋转一致），`b` 重绘，`h` 帮助，`q` 退出。
- 状态行仿 GUI：`狼方回合（红狼行动）第 N 步` / `羊方回合（黑羊行动）` / 终局原因。

### 11.2 `./build/solve_term` —— 终端解棋器（表库咨询 / 复盘点评 / 最优路线）

- 启动扫描 `--data-dir`（默认 `data/tb_test`）已求解桶并显示（当前 k=4..10 可用，
  k=11..15 产出后自动纳入，也可 `refresh` 重扫）。
- **结论**：任意局面给出 `表库结论: 狼胜（最快 N 步）/羊胜/和棋/未求解`。
  N = 从当前局面算起的半回合数（双方各走一手计 2），吃子算 1 步。
- **最优应手**：`ai`（或 `best`）列出当前回合方全部走法及后继结果，
  ★ 标记保持 DTM 最优的着法；`play` 由表库直接走出最优应手（人机对练）。
- **最优路线**：`pv [n]` 打印双方都按 DTM 最优的完整路线（默认 20 步）。
- **复盘点评**：`m a1 b2` 走子后自动点评上一步——
  `此步为最优` / `错失胜机，此步后成和棋` / `从和棋送成必败` / `妙手逆转` 等，
  并给出下一步参考（含距离）。
- **摆盘**：`setup <fen>`（FEN 与 `pos` 输出互逆）或交互逐行输入；`load <i>` / `list`
  载入预设练习局面（8 个，k=4..6 的狼胜/羊胜/速胜/消耗战）。
- 未求解局面（如初始 k=15）：结论区提示 `尚未求解`，`m` 仍可正常走子，
  `play/pv` 会明确拒绝而非瞎猜。

解棋脚本命令一览（`h` 内亦有）：

```
m a1 b2   走子（复盘时交替输入双方走法，自动点评）
ai/best   全部走法评估 + ★ 最优应手
play      表库走出最优应手
pv [n]    最优路线（默认 20 步）
setup     摆盘（FEN 或逐行）；load/list 预设；pos 打印 FEN
undo/rot/new/refresh  悔棋 / 旋转 / 初始局面 / 重扫桶
```

### 11.3 输入与输出设计的取舍（对应需求的三个问题）

1. **对手走棋怎么输入？** 三种模式，同一组命令覆盖：
   - *人机对练*：人走 `m`，表库 `play`（或 `--auto` 开局后全自动对弈）；
   - *实录复盘*：双方轮流 `m`，每步自动点评（最优/错失/逆转，含剩余步数）；
   - *摆盘评估*：`setup`/`load` 直接给结论 + `pv` 最优路线。
   坐标统一 `a1..e5`，兼容 `r1c2`、`12` 数字写法。
2. **结果怎么告诉用户？** 每次走子/查询都输出：棋盘（红 W / 青 S）+ 局面摘要
   （k、轮到谁、已走步数）+ `表库结论`（胜方 + 最快步数或和棋/未求解）+ 上一步点评
   + 当前方所有走法评估（★ 最优）。结论**绝对结局**（与哪方行棋无关），
   距离为当前方走到终局的最短（胜）/最长（败）半回合数。
3. **最优路线怎么给？** `pv` 给出双方均 DTM 最优的着法序列，每步标注后继结论与
   剩余距离；`ai` 给出的 ★ 集合即任意最优下一手（多个等价最优时全部列出）。

### 11.4 构建

设备无 make/apt，用 /tmp/gcc-local 的手工命令（同 §7 环境）：

```bash
export LD_LIBRARY_PATH=/tmp/gcc-local/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
CXX=/tmp/gcc-local/usr/bin/g++
CXXFLAGS="-std=c++20 -O3 -march=native -flto -idirafter /tmp/gcc-local/usr/include -idirafter /tmp/gcc-local/usr/include/x86_64-linux-gnu"
LDFLAGS="-flto -L/tmp/gcc-local/usr/lib/x86_64-linux-gnu -Wl,-rpath,/tmp/gcc-local/usr/lib/x86_64-linux-gnu"
$CXX $CXXFLAGS -Iinclude tools/play.cpp build/board.o $LDFLAGS -o build/play
$CXX $CXXFLAGS -Iinclude tools/solve_term.cpp build/board.o build/encode.o build/symmetry.o build/tablebase.o $LDFLAGS -o build/solve_term
```

有 make 的机器：`make play` / `make solve-term`（新目标已在 Makefile）。
