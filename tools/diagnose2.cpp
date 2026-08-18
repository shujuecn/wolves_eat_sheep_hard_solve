// 诊断工具2：对第一个 mismatch 做 BFS，打印原表库中后继树的值，定位和棋误判根源
#include "board.h"
#include "encode.h"
#include "tablebase.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <queue>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace wolves;

struct Loaded {
    uint8_t* data = nullptr;
    void load(const std::string& path, int k) {
        int fd = ::open(path.c_str(), O_RDONLY);
        struct stat st; ::fstat(fd, &st);
        uint8_t* map = (uint8_t*)::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        data = map + sizeof(TBHeader);
    }
};

struct Node {
    uint32_t wr, sr;
    bool turn;  // tablebase turn (true=sheep)
    int depth;
    uint64_t idx;
    uint8_t r, d;
};

int main() {
    init_binom();
    init_wolf_info();

    Loaded orig;
    orig.load("data/tb/wsf_tb_dtc_k04.bin", 4);

    const int k = 4;
    uint64_t sheep_combos = BINOM[22][k];

    // 找到第一个 mismatch
    uint64_t start_idx = UINT64_MAX;
    {
        Loaded retro;
        retro.load("data/tb_test/wsf_tb_dtc_k04.bin", 4);
        for (uint32_t wr = 0; wr < 2300; ++wr) {
            for (uint32_t sr = 0; sr < sheep_combos; ++sr) {
                for (int t = 0; t < 2; ++t) {
                    uint64_t idx = state_index(wr, sr, k, t);
                    if (tb_result(orig.data[idx]) != tb_result(retro.data[idx])) {
                        start_idx = idx;
                        goto found;
                    }
                }
            }
        }
    }
found:
    if (start_idx == UINT64_MAX) { std::cout << "no mismatch\n"; return 0; }

    // BFS from start_idx, print orig values up to depth 5
    std::queue<uint64_t> q;
    std::map<uint64_t,int> seen_depth;
    q.push(start_idx);
    seen_depth[start_idx] = 0;

    int printed = 0;
    while (!q.empty() && printed < 80) {
        uint64_t idx = q.front(); q.pop();
        int depth = seen_depth[idx];
        uint32_t wr = wolf_rank_from_index(idx, k);
        uint32_t sr = sheep_rank_from_index(idx, k);
        bool turn = turn_from_index(idx);
        uint8_t r = tb_result(orig.data[idx]);
        uint8_t d = tb_distance(orig.data[idx]);

        State s;
        s.turn = !turn;
        const auto& info = WOLF_INFO[wr];
        for (int wp : info.positions) s.wolf_bb |= (1u << wp);
        auto sheep = decode_sheep(info.free_list, sr, k);
        for (int sp : sheep) s.sheep_bb |= (1u << sp);

        std::cout << "d" << depth << " [" << (turn?"SHEEP":"WOLF") << "] "
                  << "wr=" << wr << " sr=" << sr
                  << " val=" << (int)r << "/" << (int)d << "\n";
        printed++;
        if (depth >= 4) continue;

        auto moves = gen_moves(s);
        for (const auto& m : moves) {
            State succ = apply(s, m);
            int succ_k = (m.captured >= 0) ? (k-1) : k;
            uint32_t nwr = encode_wolf_bb(succ.wolf_bb);
            const auto& nwi = WOLF_INFO[nwr];
            std::vector<int> nsheep;
            uint32_t bb = succ.sheep_bb;
            while (bb) { int i = std::countr_zero(bb); bb &= bb-1; nsheep.push_back(i); }
            uint32_t nsr = encode_sheep(nwi.free_list, nsheep, succ_k);
            uint64_t sidx = state_index(nwr, nsr, succ_k, !turn);
            uint8_t sv = (succ_k < 4) ? tb_pack(TB_WOLF_WIN,0) : orig.data[sidx];
            std::cout << "    -> d" << depth+1 << " [" << (!turn?"SHEEP":"WOLF") << "] "
                      << (m.captured>=0?"CAP":"mov") << " "
                      << m.from << "->" << m.to
                      << " val=" << (int)tb_result(sv) << "/" << (int)tb_distance(sv) << "\n";
            if (succ_k >= 4 && seen_depth.find(sidx) == seen_depth.end()) {
                seen_depth[sidx] = depth+1;
                q.push(sidx);
            }
        }
    }
    return 0;
}
