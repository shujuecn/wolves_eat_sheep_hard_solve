/**
 * solve_all.cpp
 *
 * 主求解入口：分桶求解、并行、断点续算
 *
 * 用法：
 *   solve_all [--data-dir <path>] [--threads N] [--block-size N]
 *             [--start-k K] [--end-k K] [--verbose] [--quiet]
 *
 * 默认 data_dir = ./data/tb
 */

#include "board.h"
#include "encode.h"
#include "solver.h"
#include "symmetry.h"
#include "tablebase.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace wolves;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --data-dir <path>   Tablebase directory (default: ./data/tb)\n"
              << "  --threads N         Number of threads (default: all cores)\n"
              << "  --block-size N      Wolf ranks per block (default: 100)\n"
              << "  --start-k K         Start from bucket k (default: 4)\n"
              << "  --end-k K           End at bucket k (default: 15)\n"
              << "  --max-iters N       Max iterations per bucket (default: 200)\n"
              << "  --no-symmetry       Disable mirror symmetry\n"
              << "  --verbose           Verbose output\n"
              << "  --quiet             Minimal output\n"
              << "  --help              Show this help\n";
}

int main(int argc, char** argv) {
    std::string data_dir = "./data/tb";
    SolverConfig config;

    int start_k = 4;
    int end_k = 15;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--block-size" && i + 1 < argc) {
            config.block_size = std::stoi(argv[++i]);
        } else if (arg == "--start-k" && i + 1 < argc) {
            start_k = std::stoi(argv[++i]);
        } else if (arg == "--end-k" && i + 1 < argc) {
            end_k = std::stoi(argv[++i]);
        } else if (arg == "--max-iters" && i + 1 < argc) {
            config.max_iterations = std::stoi(argv[++i]);
        } else if (arg == "--no-symmetry") {
            config.use_symmetry = false;
        } else if (arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--quiet") {
            config.verbose = false;
        }
    }

    if (config.num_threads == 0) {
        config.num_threads = static_cast<int>(
            std::thread::hardware_concurrency());
        if (config.num_threads == 0) config.num_threads = 4;
    }

    // 让进度输出立即刷新，避免通过管道/tee 时被缓冲导致看起来像卡住。
    std::cout << std::unitbuf;

    std::cout << "=== Wolves Eat Sheep — Hard Solve ===\n";
    std::cout << "Data dir:    " << data_dir << "\n";
    std::cout << "Threads:     " << config.num_threads << "\n";
    std::cout << "Block size:  " << config.block_size << "\n";
    std::cout << "Buckets:     k=" << start_k << " → " << end_k << "\n";
    std::cout << "Max iters:   " << config.max_iterations << "\n";
    std::cout << "Symmetry:    " << (config.use_symmetry ? "on" : "off") << "\n";
    std::cout << std::endl;

    // 初始化
    auto t0 = std::chrono::steady_clock::now();

    std::cout << "Initializing binomial coefficients...\n";
    init_binom();

    std::cout << "Initializing wolf info table...\n";
    init_wolf_info();

    std::cout << "Initializing tablebase manager...\n";
    TablebaseManager tb_manager(data_dir);

    std::cout << "Creating solver...\n";
    Solver solver(config, &tb_manager);

    // 求解
    auto t_start = std::chrono::steady_clock::now();

    for (int k = start_k; k <= end_k; ++k) {
        auto t_bucket = std::chrono::steady_clock::now();

        bool ok = solver.solve_bucket(k);

        auto t_bucket_end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            t_bucket_end - t_bucket).count();

        if (ok) {
            std::cout << "Bucket k=" << k << " done in "
                      << elapsed << "s\n";
        } else {
            std::cerr << "Bucket k=" << k << " FAILED!\n";
            return 1;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        t_end - t_start).count();

    std::cout << "\n=== All buckets solved in "
              << total_elapsed << "s ===\n";

    return 0;
}