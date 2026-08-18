#pragma once

#include "board.h"
#include "encode.h"
#include "symmetry.h"
#include "tablebase.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace wolves {

// ============================================================
// 求解器配置
// ============================================================

struct SolverConfig {
    int num_threads = 0;        // 0 = 自动检测
    int max_iterations = 200;   // 最大迭代次数（安全上限）
    int block_size = 100;       // 每个 wolf_rank 块的大小（用于并行）
    bool verbose = true;        // 详细输出
    bool use_symmetry = true;   // 使用镜像压缩
};

// ============================================================
// 走法 → 后继索引的缓存
// ============================================================

// 预先计算从每个格子的每个方向可达的后继格
struct MoveTable {
    // 简单移动：adjacent[from] = {to1, to2, to3, to4}（-1 表示无效）
    std::array<std::array<int, 4>, 25> adjacent;
    // 吃子：capture[from][dir] = {prey, jumped}（-1 表示无效）
    // capture[from][0] = 向上吃, [1] = 向下, [2] = 向左, [3] = 向右
    std::array<std::array<int, 4>, 25> capture_prey;
    std::array<std::array<int, 4>, 25> capture_jumped;

    MoveTable();
};

extern const MoveTable MOVE_TABLE;

// ============================================================
// 求解器
// ============================================================

class Solver {
public:
    Solver(const SolverConfig& config, TablebaseManager* tb_manager);

    // 求解单个桶 k
    // 前提：k-1 桶已求解（如果是 k≥4）
    bool solve_bucket(int k);

    // 求解所有桶（k=4→15，k=1..3 是平凡狼胜，无需表库）
    bool solve_all();

    // 进度
    float progress() const { return progress_.load(); }

private:
    SolverConfig config_;
    TablebaseManager* tb_manager_;

    std::atomic<float> progress_{0.0f};

    // 从 State 计算后继并更新当前状态
    // 返回 true 如果状态值发生变化
    bool evaluate_state(Tablebase& tb, int k, uint64_t idx,
                        uint32_t wolf_rank, uint32_t sheep_rank, bool turn);

    // 生成所有后继索引
    // 对于每个后继，调用 callback(successor_index, successor_k)
    void gen_successors(int k, uint32_t wolf_rank, uint32_t sheep_rank,
                        bool turn,
                        const std::function<void(uint64_t, int)>& callback);

    // 并行处理一个 wolf_rank 块
    void process_block(Tablebase& tb, int k,
                       uint32_t wolf_start, uint32_t wolf_end,
                       std::atomic<bool>& changed,
                       std::atomic<uint64_t>& solved_count);

    // 判断一个状态的终局结果
    // 返回 true 如果是终局，result 和 distance 被设置
    bool check_terminal(int k, uint32_t wolf_rank, uint32_t sheep_rank,
                        bool turn, uint8_t& result, uint8_t& distance);

    // 标记终局状态
    uint64_t mark_terminals(Tablebase& tb, int k);
};

} // namespace wolves