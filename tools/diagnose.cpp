// 诊断工具：对比 retro 与原版 k=4 表库，打印不一致状态的细节
#include "board.h"
#include "encode.h"
#include "tablebase.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace wolves;

struct Loaded {
    int fd = -1;
    uint8_t* map = nullptr;
    uint64_t size = 0;
    const uint8_t* data = nullptr;

    bool load(const std::string& path, int k) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        ::fstat(fd, &st);
        uint64_t expect = sizeof(TBHeader) + bucket_size(k);
        if ((uint64_t)st.st_size != expect) { ::close(fd); fd=-1; return false; }
        map = (uint8_t*)::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        data = map + sizeof(TBHeader);
        size = bucket_size(k);
        return true;
    }
};

int main() {
    init_binom();
    init_wolf_info();

    Loaded orig, retro;
    orig.load("data/tb/wsf_tb_dtc_k04.bin", 4);
    retro.load("data/tb_test/wsf_tb_dtc_k04.bin", 4);

    uint64_t sheep_combos = BINOM[22][4];
    int printed = 0;
    for (uint32_t wr = 0; wr < 2300 && printed < 12; ++wr) {
        const auto& info = WOLF_INFO[wr];
        for (uint32_t sr = 0; sr < sheep_combos && printed < 12; ++sr) {
            for (int t = 0; t < 2 && printed < 12; ++t) {
                uint64_t idx = state_index(wr, sr, 4, t);
                uint8_t ro = tb_result(orig.data[idx]);
                uint8_t rr = tb_result(retro.data[idx]);
                if (ro == rr) continue;

                State s;
                s.turn = !t;
                for (int wp : info.positions) s.wolf_bb |= (1u << wp);
                auto sheep = decode_sheep(info.free_list, sr, 4);
                for (int sp : sheep) s.sheep_bb |= (1u << sp);

                std::cout << "=== mismatch wr=" << wr << " sr=" << sr
                          << " turn=" << t << " (State.turn=" << (s.turn?"wolf":"sheep")
                          << ") orig=" << (int)ro << " retro=" << (int)rr << "\n";
                std::cout << to_string(s) << "\n";

                // 打印所有后继在 orig 表中的值
                auto moves = gen_moves(s);
                std::cout << "  moves=" << moves.size() << "\n";
                for (const auto& m : moves) {
                    State succ = apply(s, m);
                    int succ_k = (m.captured >= 0) ? 3 : 4;
                    uint32_t nwr = encode_wolf_bb(succ.wolf_bb);
                    const auto& nwi = WOLF_INFO[nwr];
                    std::vector<int> nsheep;
                    uint32_t bb = succ.sheep_bb;
                    while (bb) { int i = std::countr_zero(bb); bb &= bb-1; nsheep.push_back(i); }
                    uint32_t nsr = encode_sheep(nwi.free_list, nsheep, succ_k);
                    uint64_t sidx = state_index(nwr, nsr, succ_k, !t);
                    uint8_t sv = (succ_k < 4) ? tb_pack(TB_WOLF_WIN, 0)
                                              : orig.data[sidx];
                    std::cout << "    " << (m.captured>=0?"CAP ":"mov ")
                              << m.from << "->" << m.to
                              << " -> val=" << (int)tb_result(sv)
                              << "/" << (int)tb_distance(sv)
                              << " (retro=" << (int)tb_result(retro.data[sidx]) << ")\n";
                }
                printed++;
            }
        }
    }
    return 0;
}
