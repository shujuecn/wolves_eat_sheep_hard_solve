#pragma once

#include "board.h"
#include "encode.h"
#include "tablebase.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace wolves {

// ============================================================
// 预计算移动表（快速后继 / 前驱生成，全部为分配无关的紧凑结构）
// ============================================================

// 狼一步移动：from -> to（吃子时 to 为被吃羊格），jumped_pos 为被跳过的格
struct WolfMove {
    int from_pos;          // 狼起始全局坐标
    int to_pos;            // 狼目标全局坐标（吃子时 = 被吃羊格）
    int jumped_pos;        // -1 表示普通移动，否则为中间跳过格
    uint32_t new_wolf_rank;
};

// 狼普通移动的反向边：当前 wolf_rank 的某个狼从 cur_pos 退回到 prev_pos
struct RevWolfSimple {
    uint32_t prev_wolf_rank;  // 前驱 wolf_rank
    int cur_pos;              // 当前状态下狼的位置
    int prev_pos;             // 前驱状态下狼的位置
};

struct RetroMoveTable {
    // 正向狼移动（普通 + 吃子），按 wolf_rank 索引
    std::vector<std::vector<WolfMove>> wolf_moves;

    // 反向狼普通移动（同桶内：羊回合状态 -> 狼回合前驱），按 wolf_rank 索引
    std::vector<std::vector<RevWolfSimple>> rev_wolf_simple;

    // 羊邻接表：sheep_adj[wolf_rank][free_idx] = 相邻空闲格 free_idx 列表
    std::vector<std::vector<std::vector<int>>> sheep_adj;

    // 狼可移动目标：wolf_adj_free[wolf_rank][wolf_idx] = 相邻非狼格 free_idx 列表
    // 用于终局判定：某狼的所有相邻非狼格都被羊占 -> 该狼被堵死
    std::vector<std::array<std::vector<int>, 3>> wolf_adj_free;

    void build();
};

extern RetroMoveTable RETRO_MOVE_TABLE;

// ============================================================
// 逆向分析求解器配置
// ============================================================

struct RetroSolverConfig {
    int num_threads = 0;         // 0 = 自动检测
    bool verbose = true;
    bool use_symmetry = true;    // 保留字段；当前实现不做镜像压缩
    size_t chunk_size = 65536;   // 传播阶段每个并行块包含的状态数
};

// ============================================================
// 逆向分析求解器
// ============================================================

class RetrogradeSolver {
public:
    RetrogradeSolver(const RetroSolverConfig& config,
                     TablebaseManager* tb_manager);

    bool solve_bucket(int k);
    bool solve_all();

    float progress() const { return progress_.load(); }

private:
    RetroSolverConfig config_;
    TablebaseManager* tb_manager_;
    std::atomic<float> progress_{0.0f};

    // ---- 计数器：cnt[s] = “尚未被判为 LOSS(P) 的后继数” ----
    // 初始为 0（mmap 匿名页天然清零），init 阶段为每个未决状态显式赋值。
    uint8_t* counters_ = nullptr;
    uint64_t counters_size_ = 0;

    // ---- 跨桶吃子 LOSS 的最大距离（仅狼回合未决状态使用）----
    // 狼回合 LOSS 距离 = 1 + max(同桶后继距离, 吃子到 k-1 的 LOSS 距离)。
    // 吃子后继在 init 阶段即已知（k-1 表已完成），存入此数组，
    // 传播阶段在同桶计数归零时与 nd 取 max。
    uint8_t* loss_dists_ = nullptr;
    uint64_t loss_dists_size_ = 0;

    // ---- 距离桶（0..63）：按最优 DTM 分层处理 ----
    std::vector<std::vector<uint32_t>> buckets_;

    bool alloc_counters(uint64_t size);
    void free_counters();

    // init 阶段（Phase 1：狼回合；Phase 2：羊回合）
    void init_wolf_range(Tablebase& tb, int k,
                         uint32_t wr_start, uint32_t wr_end,
                         std::vector<std::vector<uint32_t>>& local_buckets);
    void init_sheep_range(Tablebase& tb, int k,
                          uint32_t wr_start, uint32_t wr_end,
                          std::vector<std::vector<uint32_t>>& local_buckets);

    // 传播阶段：处理一个距离桶的一段，产生下一距离桶的新状态。
    // out 元素为 (状态索引, 该状态的最终距离)：LOSS 路径的实际距离可能
    // 大于 next_dist（并入跨桶捕获的 max_loss），必须按实际距离入桶。
    void propagate_range(Tablebase& tb, int k,
                         const std::vector<uint32_t>& cur,
                         uint32_t lo, uint32_t hi, int wave_dist, int next_dist,
                         std::vector<std::pair<uint32_t, uint8_t>>& out);
};

} // namespace wolves
