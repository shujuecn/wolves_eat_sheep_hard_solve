// stat_tb.cpp — 表库结果分布统计（只读、顺序扫描，几乎不耗 CPU/内存）
//
// 用途：统计已完成后桶中「狼胜 / 羊胜 / 和棋」的局面占比，并按"轮到狼 / 轮到羊"
//       分别给出。只扫描表头 completed==1 的完成桶，正在求解的桶自动跳过，
//       因此可以随时在求解进程运行期间安全使用。
//
// 用法：
//   ./build/stat_tb [--data-dir data/ws_tb_dtc_260819] [--raw]
//     --raw  每个 k 额外打印精确原始计数（小概率项不再被百分比舍入吞掉）
//
// 实现说明：每条目 1 字节（bits0-1 结果，bits2-7 距离）。索引 idx 的奇偶即回合
// 位（state_index = (...)*2 + turn，turn=1 表示羊回合 → 奇数索引 = 轮到羊）。
// 纯顺序只读映射 + 位运算，约 17GB 数据不到 1 分钟；不写任何文件。
// 跨平台：文件映射经 include/platform.h 统一封装（Windows/macOS/Linux）。
#include "tablebase.h"
#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace wolves;

namespace {

struct Counts {
    uint64_t res[3] = {0, 0, 0};
};

} // namespace

int main(int argc, char** argv) {
    std::string dir = kDefaultDataDir;
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (std::strcmp(argv[i], "--raw") == 0) raw = true;
        else { std::fprintf(stderr, "未知参数: %s\n", argv[i]); return 1; }
    }

    uint64_t gW[3] = {0, 0, 0};   // 轮到狼，按结果
    uint64_t gS[3] = {0, 0, 0};   // 轮到羊，按结果
    uint64_t gTotal = 0, gBytes = 0, gUnknown = 0;
    int done = 0;

    for (int k = 4; k <= 15; ++k) {
        uint64_t fsz = 0;
        const uint8_t* map = static_cast<const uint8_t*>(
            os_map_file_read(tb_bucket_path(dir, k), fsz));
        if (!map) continue;
        if (fsz < 64 || map[24] != 1) {  // completed!=1 跳过
            os_unmap(const_cast<uint8_t*>(map), fsz);
            continue;
        }
        uint64_t entries = fsz - 64;

        Counts w, s;
        uint64_t unk = 0;
        for (uint64_t i = 0; i < entries; ++i) {
            uint8_t r = map[64 + i] & TB_RESULT_MASK;
            bool sheepTurn = (i & 1u) != 0;
            if (r == TB_UNKNOWN) { ++unk; continue; }
            if (sheepTurn) s.res[r]++;
            else w.res[r]++;
        }
        uint64_t W = w.res[0] + w.res[1] + w.res[2];
        uint64_t S = s.res[0] + s.res[1] + s.res[2];
        uint64_t tot = W + S;
        if (raw) {
            std::printf("k=%2d 原始: 轮到狼[狼胜=%llu 羊胜=%llu 和=%llu] 轮到羊[狼胜=%llu 羊胜=%llu 和=%llu] 未解=%llu\n",
                        k, (unsigned long long)w.res[0], (unsigned long long)w.res[1],
                        (unsigned long long)w.res[2], (unsigned long long)s.res[0],
                        (unsigned long long)s.res[1], (unsigned long long)s.res[2],
                        (unsigned long long)unk);
        }
        std::printf("k=%2d  条目 %9.1fM | 轮到狼: 狼胜%6.1f%% 羊胜%6.1f%% 和%6.1f%% | 轮到羊: 狼胜%6.1f%% 羊胜%6.1f%% 和%6.1f%%\n",
                    k, entries / 1e6,
                    100.0 * w.res[0] / W, 100.0 * w.res[1] / W, 100.0 * w.res[2] / W,
                    100.0 * s.res[0] / S, 100.0 * s.res[1] / S, 100.0 * s.res[2] / S);
        for (int r = 0; r < 3; ++r) { gW[r] += w.res[r]; gS[r] += s.res[r]; }
        gTotal += tot;
        gBytes += entries;
        gUnknown += unk;
        ++done;
        os_unmap(const_cast<uint8_t*>(map), fsz);
    }

    if (done == 0) {
        std::printf("（无已完成桶：%s 下未找到 completed==1 的表库文件）\n", dir.c_str());
        return 1;
    }
    std::printf("\n===== 合计（%d 个完成桶，%llu 局面，%.2f GB） =====\n",
                done, (unsigned long long)gTotal, gBytes / 1e9);
    uint64_t gAll[3] = {gW[0] + gS[0], gW[1] + gS[1], gW[2] + gS[2]};
    std::printf("全部局面: 狼胜 %.2f%% | 羊胜 %.2f%% | 和棋 %.2f%% | 未知 %.4f%%\n",
                100.0 * gAll[0] / gTotal, 100.0 * gAll[1] / gTotal,
                100.0 * gAll[2] / gTotal, 100.0 * gUnknown / gTotal);
    std::printf("轮到狼:   狼胜 %.2f%% | 羊胜 %.2f%% | 和棋 %.2f%%\n",
                100.0 * gW[0] / gTotal, 100.0 * gW[1] / gTotal, 100.0 * gW[2] / gTotal);
    std::printf("轮到羊:   狼胜 %.2f%% | 羊胜 %.2f%% | 和棋 %.2f%%\n",
                100.0 * gS[0] / gTotal, 100.0 * gS[1] / gTotal, 100.0 * gS[2] / gTotal);
    return 0;
}