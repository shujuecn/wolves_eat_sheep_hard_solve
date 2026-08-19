"""
hard_solve_fast.py — 优化版 Python 硬解引擎

关键优化：
1. 预计算所有后继索引（每桶一次性计算，存为 numpy 数组）
2. 使用 worklist 逆向传播（只处理新求解的状态）
3. 使用 numpy uint8 数组加速批量操作

用法：
    python3 hard_solve_fast.py --start-k 4 --end-k 6
"""

import sys
import os
import struct
import time
import math
from collections import deque
from array import array
from typing import List, Tuple, Optional

# ============================================================
# 常量
# ============================================================

BOARD_SIZE = 5
TOTAL_CELLS = 25

DIRS = [(-1, 0), (1, 0), (0, -1), (0, 1)]

WOLF_WIN = 0
SHEEP_WIN = 1
DRAW = 2
UNKNOWN = 3

# ============================================================
# 二项式系数
# ============================================================

def init_binom(max_n: int = 25):
    C = [[0] * (max_n + 1) for _ in range(max_n + 1)]
    for n in range(max_n + 1):
        C[n][0] = C[n][n] = 1
        for k in range(1, n):
            C[n][k] = C[n-1][k-1] + C[n-1][k]
    return C

BINOM = init_binom(25)

# ============================================================
# 组合编码
# ============================================================

def encode_combination(sorted_pos: List[int], k: int) -> int:
    rank = 0
    for i, p in enumerate(sorted_pos):
        rank += BINOM[p][i + 1]
    return rank

def decode_combination(rank: int, n: int, k: int) -> List[int]:
    result = []
    pos = n - 1
    for i in range(k, 0, -1):
        while pos >= i - 1 and BINOM[pos][i] > rank:
            pos -= 1
        result.append(pos)
        rank -= BINOM[pos][i]
        pos -= 1
    result.reverse()
    return result

def encode_wolf(wolf_positions: List[int]) -> int:
    return encode_combination(sorted(wolf_positions), 3)

def decode_wolf(rank: int) -> List[int]:
    return decode_combination(rank, 25, 3)

# 预计算 wolf 信息
def build_wolf_info():
    free_lists = []
    g2f_maps = []
    wolf_pos_list = []
    for rank in range(2300):
        wp = decode_wolf(rank)
        ws = set(wp)
        wolf_pos_list.append(wp)
        free = []
        g2f = [-1] * 25
        for c in range(25):
            if c not in ws:
                g2f[c] = len(free)
                free.append(c)
        free_lists.append(free)
        g2f_maps.append(g2f)
    return wolf_pos_list, free_lists, g2f_maps

WOLF_POS, FREE_LISTS, G2F = build_wolf_info()

def encode_sheep(sheep_positions: List[int], wolf_rank: int, k: int) -> int:
    if k == 0:
        return 0
    g2f = G2F[wolf_rank]
    mapped = sorted(g2f[p] for p in sheep_positions)
    return encode_combination(mapped, k)

def decode_sheep(rank: int, wolf_rank: int, k: int) -> List[int]:
    if k == 0:
        return []
    free = FREE_LISTS[wolf_rank]
    mapped = decode_combination(rank, 22, k)
    return sorted(free[i] for i in mapped)

def state_index(wr: int, sr: int, k: int, turn: bool) -> int:
    sc = BINOM[22][k]
    return (wr * sc + sr) * 2 + (1 if turn else 0)

def bucket_size(k: int) -> int:
    return 2300 * BINOM[22][k] * 2

# ============================================================
# 走法表
# ============================================================

def build_move_table():
    adj = [[-1]*4 for _ in range(25)]
    cap_prey = [[-1]*4 for _ in range(25)]
    cap_jumped = [[-1]*4 for _ in range(25)]
    for r in range(5):
        for c in range(5):
            frm = r*5+c
            for d, (dr, dc) in enumerate(DIRS):
                nr, nc = r+dr, c+dc
                if 0 <= nr < 5 and 0 <= nc < 5:
                    adj[frm][d] = nr*5+nc
                    pr, pc = r+2*dr, c+2*dc
                    if 0 <= pr < 5 and 0 <= pc < 5:
                        cap_prey[frm][d] = pr*5+pc
                        cap_jumped[frm][d] = nr*5+nc
    return adj, cap_prey, cap_jumped

ADJ, CAP_PREY, CAP_JUMPED = build_move_table()

# ============================================================
# 终局判定
# ============================================================

def is_terminal(wr: int, sr: int, k: int) -> Tuple[bool, int, int]:
    if k < 4:
        return True, WOLF_WIN, 0
    wp = WOLF_POS[wr]
    sp = decode_sheep(sr, wr, k)
    ss = set(sp)
    ws = set(wp)
    occ = ss | ws
    all_blocked = True
    for w in wp:
        for d in range(4):
            a = ADJ[w][d]
            if a >= 0 and a not in occ:
                all_blocked = False
                break
        if not all_blocked:
            break
    if all_blocked:
        return True, SHEEP_WIN, 0
    return False, UNKNOWN, 0

# ============================================================
# 预计算后继列表
# ============================================================

def precompute_successors(k: int):
    """
    为桶 k 预计算所有状态的后继索引列表。
    返回：succ_counts (array of uint8), succ_data (array of int64)
    
    succ_counts[i] = 状态 i 的后继数量
    succ_data 依次存储所有后继索引
    后继在 k-1 桶的索引加上 (1<<63) 标记
    """
    sc = BINOM[22][k]
    total = 2300 * sc * 2
    
    # 首先计算每个状态的后继数量
    counts = array('I', [0]) * total  # 使用 Python list 逐步构建
    
    # 使用 list 逐步构建，后续转 array
    count_list = [0] * total
    all_succs = []
    
    for wr in range(2300):
        wp = WOLF_POS[wr]
        ws = set(wp)
        
        for sr in range(sc):
            sp = decode_sheep(sr, wr, k)
            ss = set(sp)
            occ = ws | ss
            
            # 狼回合 (turn=False)
            idx_w = state_index(wr, sr, k, False)
            succs_w = []
            
            for w in wp:
                for d in range(4):
                    # 简单移动
                    to = ADJ[w][d]
                    if to >= 0 and to not in occ:
                        nw = sorted((to if p == w else p) for p in wp)
                        nwr = encode_wolf(nw)
                        nsr = encode_sheep(sp, nwr, k)
                        nidx = state_index(nwr, nsr, k, True)
                        succs_w.append(nidx)
                    
                    # 吃子
                    prey = CAP_PREY[w][d]
                    jumped = CAP_JUMPED[w][d]
                    if prey >= 0 and prey in ss and jumped not in occ:
                        nw = sorted((prey if p == w else p) for p in wp)
                        nwr = encode_wolf(nw)
                        nsp = sorted(p for p in sp if p != prey)
                        nsr = encode_sheep(nsp, nwr, k - 1)
                        nidx = state_index(nwr, nsr, k - 1, True)
                        succs_w.append(nidx | (1 << 63))  # 标记为 k-1 桶
            
            count_list[idx_w] = len(succs_w)
            all_succs.extend(succs_w)
            
            # 羊回合 (turn=True)
            idx_s = state_index(wr, sr, k, True)
            succs_s = []
            
            for s in sp:
                for d in range(4):
                    to = ADJ[s][d]
                    if to >= 0 and to not in occ:
                        nsp = sorted((to if p == s else p) for p in sp)
                        nsr = encode_sheep(nsp, wr, k)
                        nidx = state_index(wr, nsr, k, False)
                        succs_s.append(nidx)
            
            count_list[idx_s] = len(succs_s)
            all_succs.extend(succs_s)
    
    # 构建偏移数组
    offsets = [0]
    for c in count_list:
        offsets.append(offsets[-1] + c)
    
    return count_list, offsets, all_succs


# ============================================================
# 表库文件
# ============================================================

def tb_pack(result: int, distance: int) -> int:
    return (result & 0x03) | ((distance & 0x3F) << 2)

def tb_unpack(entry: int) -> Tuple[int, int]:
    return entry & 0x03, (entry >> 2) & 0x3F

def write_tb(path: str, k: int, data: bytes):
    total = len(data)
    with open(path, 'wb') as f:
        hdr = bytearray(64)
        hdr[0:4] = b'WSTB'
        hdr[4] = 1
        hdr[5] = k
        hdr[6] = 8
        hdr[8:12] = struct.pack('<I', 2300)
        hdr[12:16] = struct.pack('<I', BINOM[22][k])
        hdr[16:24] = struct.pack('<Q', total)
        hdr[24] = 1
        f.write(hdr)
        f.write(data)

def read_tb(path: str) -> Optional[Tuple[int, bytes]]:
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        hdr = f.read(64)
        if len(hdr) < 64 or hdr[0:4] != b'WSTB':
            return None
        k = hdr[5]
        data = f.read()
    return k, data


# ============================================================
# 快速求解器（worklist 传播）
# ============================================================

class FastSolver:
    def __init__(self, data_dir: str = "./data/tb"):
        self.data_dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self._cache = {}  # (k, idx) -> entry
    
    def _path(self, k: int) -> str:
        return os.path.join(self.data_dir, f"wsf_tb_dtc_k{k:02d}.bin")
    
    def solve_bucket(self, k: int, max_iters: int = 200) -> bool:
        if k < 4:
            print(f"Bucket k={k} trivial, skipping.")
            return True
        
        path = self._path(k)
        result = read_tb(path)
        if result is not None and len(result[1]) > 0:
            # 检查是否完成
            with open(path, 'rb') as f:
                hdr = f.read(64)
                if hdr[24] == 1:
                    print(f"Bucket k={k} already completed, skipping.")
                    return True
        
        sc = BINOM[22][k]
        total = bucket_size(k)
        print(f"\n=== Solving bucket k={k} ===")
        print(f"  Sheep combos: {sc:,}")
        print(f"  Total entries: {total:,} ({total // 1024 // 1024} MB)")
        
        # 初始化表
        print("  Initializing table...")
        tb = bytearray([tb_pack(UNKNOWN, 0)] * total)
        
        # 标记终局 & 建立 worklist
        print("  Marking terminals and building worklist...")
        worklist = deque()
        terminal_count = 0
        
        for wr in range(2300):
            for sr in range(sc):
                for t in range(2):
                    turn = (t == 1)
                    is_term, result, dist = is_terminal(wr, sr, k)
                    if is_term:
                        idx = state_index(wr, sr, k, turn)
                        tb[idx] = tb_pack(result, dist)
                        worklist.append(idx)
                        terminal_count += 1
        
        print(f"  Terminals: {terminal_count:,}")
        
        if not worklist:
            print("  No terminals found!")
            return False
        
        # 预计算后继（仅在第一次迭代时使用）
        print("  Precomputing successors...")
        t0 = time.time()
        count_list, offsets, all_succs = precompute_successors(k)
        print(f"  Precomputed in {time.time()-t0:.1f}s")
        
        # 预计算前驱（反向边）
        # 对于每个状态，找到所有能到达它的前驱状态
        print("  Building predecessor graph...")
        t0 = time.time()
        pred_counts = [0] * total
        for idx in range(total):
            for si in range(offsets[idx], offsets[idx + 1]):
                succ = all_succs[si]
                is_prev_k = (succ >> 63) != 0
                succ_idx = succ & ((1 << 63) - 1)
                if not is_prev_k:
                    pred_counts[succ_idx] += 1
        
        # 分配前驱数组
        pred_offsets = [0]
        for c in pred_counts:
            pred_offsets.append(pred_offsets[-1] + c)
        pred_data = [0] * pred_offsets[-1]
        pred_fill = pred_offsets[:-1].copy()  # 当前填充位置
        
        for idx in range(total):
            for si in range(offsets[idx], offsets[idx + 1]):
                succ = all_succs[si]
                is_prev_k = (succ >> 63) != 0
                succ_idx = succ & ((1 << 63) - 1)
                if not is_prev_k:
                    pos = pred_fill[succ_idx]
                    pred_data[pos] = idx
                    pred_fill[succ_idx] = pos + 1
        
        print(f"  Predecessor graph built in {time.time()-t0:.1f}s")
        print(f"  Total edges: {pred_offsets[-1]:,}")
        
        # Worklist 传播
        print("  Propagating...")
        t0 = time.time()
        solved_count = len(worklist)
        last_report = 0
        
        while worklist:
            idx = worklist.popleft()
            my_entry = tb[idx]
            my_result, my_dist = tb_unpack(my_entry)
            
            # 遍历所有前驱
            for pi in range(pred_offsets[idx], pred_offsets[idx + 1]):
                pred_idx = pred_data[pi]
                pred_entry = tb[pred_idx]
                pred_result, pred_dist = tb_unpack(pred_entry)
                if pred_result != UNKNOWN:
                    continue
                
                # 判断前驱是否能被求解
                # pred_idx → wolf_rank, sheep_rank, turn
                pred_turn = (pred_idx & 1) == 1
                
                if pred_turn:
                    # 羊回合前驱：羊想找 SHEEP_WIN
                    if my_result == SHEEP_WIN:
                        tb[pred_idx] = tb_pack(SHEEP_WIN, min(63, my_dist + 1))
                        worklist.append(pred_idx)
                        solved_count += 1
                    elif my_result == WOLF_WIN:
                        # 检查是否所有后继都是 WOLF_WIN
                        all_wolf_win = True
                        max_dist = my_dist
                        for si in range(offsets[pred_idx], offsets[pred_idx + 1]):
                            succ = all_succs[si]
                            is_prev = (succ >> 63) != 0
                            s_idx = succ & ((1 << 63) - 1)
                            if is_prev:
                                # k-1 桶
                                s_entry = self._get_cached(s_idx, k - 1)
                            else:
                                s_entry = tb[s_idx]
                            sr, sd = tb_unpack(s_entry)
                            if sr != WOLF_WIN:
                                all_wolf_win = False
                                break
                            max_dist = max(max_dist, sd)
                        if all_wolf_win:
                            tb[pred_idx] = tb_pack(WOLF_WIN, min(63, max_dist + 1))
                            worklist.append(pred_idx)
                            solved_count += 1
                else:
                    # 狼回合前驱：狼想找 WOLF_WIN
                    if my_result == WOLF_WIN:
                        tb[pred_idx] = tb_pack(WOLF_WIN, min(63, my_dist + 1))
                        worklist.append(pred_idx)
                        solved_count += 1
                    elif my_result == SHEEP_WIN:
                        # 检查是否所有后继都是 SHEEP_WIN
                        all_sheep_win = True
                        max_dist = my_dist
                        for si in range(offsets[pred_idx], offsets[pred_idx + 1]):
                            succ = all_succs[si]
                            is_prev = (succ >> 63) != 0
                            s_idx = succ & ((1 << 63) - 1)
                            if is_prev:
                                s_entry = self._get_cached(s_idx, k - 1)
                            else:
                                s_entry = tb[s_idx]
                            sr, sd = tb_unpack(s_entry)
                            if sr != SHEEP_WIN:
                                all_sheep_win = False
                                break
                            max_dist = max(max_dist, sd)
                        if all_sheep_win:
                            tb[pred_idx] = tb_pack(SHEEP_WIN, min(63, max_dist + 1))
                            worklist.append(pred_idx)
                            solved_count += 1
            
            # 进度报告
            if solved_count - last_report >= 100000:
                elapsed = time.time() - t0
                pct = 100.0 * solved_count / total
                print(f"    Solved: {solved_count:,}/{total:,} ({pct:.1f}%) "
                      f"in {elapsed:.1f}s, queue: {len(worklist):,}")
                last_report = solved_count
        
        elapsed = time.time() - t0
        print(f"  Propagation done in {elapsed:.1f}s")
        print(f"  Total solved: {solved_count:,}")
        
        # 剩余 UNKNOWN → DRAW
        print("  Setting remaining as DRAW...")
        draw_count = 0
        for idx in range(total):
            result, _ = tb_unpack(tb[idx])
            if result == UNKNOWN:
                tb[idx] = tb_pack(DRAW, 0)
                draw_count += 1
        print(f"  Draws: {draw_count:,}")
        
        # 保存
        print(f"  Saving to {path}...")
        write_tb(path, k, bytes(tb))
        print(f"  Bucket k={k} completed.")
        
        # 缓存此桶的数据
        self._tb_cache = tb
        self._tb_cache_k = k
        
        return True
    
    def _get_cached(self, idx: int, k: int) -> int:
        """从 k 桶获取条目（k 桶必须是已求解的前一个桶）"""
        if hasattr(self, '_tb_cache') and self._tb_cache_k == k:
            return self._tb_cache[idx]
        
        path = self._path(k)
        if not os.path.exists(path):
            return tb_pack(UNKNOWN, 0)
        
        with open(path, 'rb') as f:
            f.seek(64 + idx)
            return f.read(1)[0]
    
    def solve_all(self, start_k: int = 4, end_k: int = 15):
        for k in range(start_k, end_k + 1):
            t0 = time.time()
            ok = self.solve_bucket(k)
            elapsed = time.time() - t0
            if ok:
                print(f"  → {elapsed:.1f}s\n")
            else:
                print(f"  → FAILED\n")
                return False
        return True


# ============================================================
# 主入口
# ============================================================

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Wolves Eat Sheep — Fast Hard Solve")
    parser.add_argument('--start-k', type=int, default=4)
    parser.add_argument('--end-k', type=int, default=6)
    parser.add_argument('--data-dir', type=str, default='./data/tb')
    args = parser.parse_args()
    
    print("=== Wolves Eat Sheep — Fast Hard Solve ===")
    print(f"Data dir: {args.data_dir}")
    print(f"Buckets:  k={args.start_k} → {args.end_k}")
    print()
    
    solver = FastSolver(args.data_dir)
    solver.solve_all(args.start_k, args.end_k)
    print("\nDone.")

if __name__ == '__main__':
    main()