#!/usr/bin/env bash
# ============================================================
# run_solve.sh — 狼羊棋硬解全量求解脚本
# ============================================================
# 用法：
#   ./run_solve.sh              # 完整求解 k=4→15
#   ./run_solve.sh --resume     # 断点续算（跳过已完成桶）
#   ./run_solve.sh --dry-run    # 只打印规模预算，不执行
#   ./run_solve.sh --start-k 7  # 从 k=7 开始
#
# 日志：solve_log_YYYYMMDD_HHMMSS.log
# 摘要：solve_summary.txt
# ============================================================

set -euo pipefail

# 保证日志及子进程按 UTF-8 输出（环境未设置时回退到 C.UTF-8）
export LANG="${LANG:-C.UTF-8}"

# ---- 颜色 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
RESET='\033[0m'

# ---- 路径 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/build"
DATA_DIR="$SCRIPT_DIR/data/tb"
LOCAL_PREFIX="/tmp/gcc-local"

# ---- 可执行文件 ----
SOLVE_ALL="$BUILD_DIR/solve_all"
DUMP_BOOK="$BUILD_DIR/dump_opening_book"
VERIFY="$BUILD_DIR/verify"

# ---- 日志 ----
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="$SCRIPT_DIR/solve_log_${TIMESTAMP}.log"
SUMMARY_FILE="$SCRIPT_DIR/solve_summary.txt"

# ---- 默认参数 ----
THREADS=$(nproc 2>/dev/null || echo 80)
BLOCK_SIZE=100
MAX_ITERS=200
START_K=4
END_K=15
DRY_RUN=false

# ---- 桶规模表（entries） ----
declare -A BUCKET_ENTRIES=(
    [4]=33649000
    [5]=121136400
    [6]=343219800
    [7]=784502400
    [8]=1470942000
    [9]=2288132000
    [10]=2974571600
    [11]=3244987200
    [12]=2974571600
    [13]=2288132000
    [14]=1470942000
    [15]=784502400
)

# ---- 解析参数 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --resume) shift ;;  # 默认行为就是跳过已完成的，此参数仅作兼容
        --dry-run) DRY_RUN=true; shift ;;
        --threads) THREADS="$2"; shift 2 ;;
        --start-k) START_K="$2"; shift 2 ;;
        --end-k) END_K="$2"; shift 2 ;;
        --block-size) BLOCK_SIZE="$2"; shift 2 ;;
        --max-iters) MAX_ITERS="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --dry-run       Print scale budget only, no execution"
            echo "  --threads N     Number of threads (default: all cores = $(nproc))"
            echo "  --start-k K     Start from bucket k (default: 4)"
            echo "  --end-k K       End at bucket k (default: 15)"
            echo "  --block-size N  Wolf ranks per block (default: 100)"
            echo "  --max-iters N   Max iterations per bucket (default: 200)"
            echo ""
            echo "Already-completed buckets are automatically skipped."
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ============================================================
# 工具函数
# ============================================================

log() {
    local msg="[$(date '+%H:%M:%S')] $*"
    echo -e "$msg" | tee -a "$LOG_FILE"
}

log_color() {
    local color="$1"
    shift
    local msg="[$(date '+%H:%M:%S')] $*"
    # 终端显示彩色，落盘只写纯文本，避免 ANSI 转义码混进 UTF-8 日志
    printf '%b\n' "${color}${msg}${RESET}"
    printf '%s\n' "$msg" >> "$LOG_FILE"
}

banner() {
    local msg="$1"
    local line="============================================================"
    echo "" | tee -a "$LOG_FILE"
    log_color "$BOLD" "$line"
    log_color "$BOLD" "  $msg"
    log_color "$BOLD" "$line"
    echo "" | tee -a "$LOG_FILE"
}

format_seconds() {
    local s=$1
    local h=$((s / 3600))
    local m=$(((s % 3600) / 60))
    local sec=$((s % 60))
    if [ $h -gt 0 ]; then
        printf "%dh%02dm%02ds" $h $m $sec
    elif [ $m -gt 0 ]; then
        printf "%dm%02ds" $m $sec
    else
        printf "%ds" $sec
    fi
}

format_size() {
    local bytes=$1
    if [ "$bytes" -ge 1073741824 ]; then
        echo "$(echo "scale=2; $bytes / 1073741824" | bc) GB"
    elif [ "$bytes" -ge 1048576 ]; then
        echo "$((bytes / 1048576)) MB"
    elif [ "$bytes" -ge 1024 ]; then
        echo "$((bytes / 1024)) KB"
    else
        echo "${bytes} B"
    fi
}

version_of() {
    local out
    out="$("$@" 2>/dev/null | head -n 1 || true)"
    if [ -n "$out" ]; then
        printf '%s' "$out"
    else
        printf 'unknown'
    fi
}

# ============================================================
# 环境检查
# ============================================================

check_env() {
    banner "环境检查"

    # 检查本地 g++
    if [ -x "$LOCAL_PREFIX/usr/bin/g++" ]; then
        export PATH="$LOCAL_PREFIX/usr/bin:$PATH"
        export LD_LIBRARY_PATH="$LOCAL_PREFIX/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
        log_color "$GREEN" "✓ 本地 g++: $(version_of g++ --version)"
    else
        log_color "$YELLOW" "⚠ 未找到本地 g++，使用系统编译器: $(which g++ 2>/dev/null || echo 'N/A')"
    fi

    # 检查 cmake
    if command -v cmake &>/dev/null; then
        log_color "$GREEN" "✓ cmake: $(version_of cmake --version)"
    else
        log_color "$RED" "✗ cmake 未找到"
        exit 1
    fi

    # 检查 make
    if command -v make &>/dev/null; then
        log_color "$GREEN" "✓ make: $(version_of make --version)"
    else
        log_color "$RED" "✗ make 未找到"
        exit 1
    fi

    # 系统信息
    log "  主机:   $(hostname)"
    log "  CPU:    $(nproc) 核"
    log "  内存:   $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo 'N/A')"
    log "  磁盘:   $(df -h "$SCRIPT_DIR" 2>/dev/null | awk 'NR==2{print $4}' || echo 'N/A') 可用"
    log "  线程数: $THREADS"
    log "  块大小: $BLOCK_SIZE"

    # 创建数据目录
    mkdir -p "$DATA_DIR"
    mkdir -p "$BUILD_DIR"
}

# ============================================================
# 编译
# ============================================================

build() {
    banner "编译"

    log "  编译器: $(which g++)"
    log "  标准:    C++20"
    log "  优化:    -O3 -march=native -flto"

    if make -j"$THREADS" all 2>&1 | tee -a "$LOG_FILE"; then
        log_color "$GREEN" "✓ 编译成功"
    else
        log_color "$RED" "✗ 编译失败"
        exit 1
    fi

    # 验证可执行文件
    for exe in "$SOLVE_ALL" "$DUMP_BOOK" "$VERIFY"; do
        if [ -x "$exe" ]; then
            log "  $(basename "$exe"): $(du -h "$exe" 2>/dev/null | cut -f1)"
        else
            log_color "$RED" "✗ 缺少: $exe"
            exit 1
        fi
    done
}

# ============================================================
# 规模预算
# ============================================================

print_budget() {
    banner "规模预算"

    local total_entries=0

    printf "  %-4s  %16s  %12s  %10s\n" "k" "Entries" "Size" "Status"
    printf "  %-4s  %16s  %12s  %10s\n" "----" "----------------" "------------" "----------"

    for k in $(seq 4 15); do
        local entries=${BUCKET_ENTRIES[$k]}
        local size_str=$(format_size $entries)
        local status=""
        if is_bucket_done "$k"; then
            status="${GREEN}✓ done${RESET}"
        elif [ -f "$DATA_DIR/wsf_tb_dtc_k$(printf "%02d" $k).bin" ]; then
            status="${YELLOW}partial${RESET}"
        else
            status="-"
        fi
        printf "  %-4s  %'16d  %12s  " "$k" "$entries" "$size_str"
        echo -e "$status"
        total_entries=$((total_entries + entries))
    done

    local total_gb=$(echo "scale=2; $total_entries / 1073741824" | bc)
    echo ""
    log "  总计: $(printf "%'d" $total_entries) entries ≈ ${total_gb} GB"
    log "  线程: $THREADS  |  块大小: $BLOCK_SIZE  |  最大迭代: $MAX_ITERS/桶"
}

# ============================================================
# 检查桶是否已完成
# ============================================================

is_bucket_done() {
    local k=$1
    local tb_file="$DATA_DIR/wsf_tb_dtc_k$(printf "%02d" $k).bin"

    if [ ! -f "$tb_file" ]; then
        return 1
    fi

    # 检查 magic "WSTB"
    local magic
    magic=$(dd if="$tb_file" bs=1 count=4 2>/dev/null | tr -d '\0')
    if [ "$magic" != "WSTB" ]; then
        # 文件损坏
        return 1
    fi

    # 读 completed 字节 (TBHeader offset 24)
    local completed
    completed=$(dd if="$tb_file" bs=1 skip=24 count=1 2>/dev/null | od -An -td1 | tr -d ' ')
    if [ "$completed" = "1" ]; then
        return 0
    fi
    return 1
}

# ============================================================
# 求解单个桶
# ============================================================

solve_one_bucket() {
    local k=$1
    local tb_file="$DATA_DIR/wsf_tb_dtc_k$(printf "%02d" $k).bin"
    local entries=${BUCKET_ENTRIES[$k]}
    local size_str=$(format_size $entries)

    banner "求解 k=$k  ($size_str)"

    if is_bucket_done "$k"; then
        local fsize
        fsize=$(du -h "$tb_file" 2>/dev/null | cut -f1)
        log_color "$GREEN" "  ✓ k=$k 已完成，跳过 ($fsize)"
        return 0
    fi

    # 如果文件存在但未完成，说明上次中断了，删除重来
    if [ -f "$tb_file" ]; then
        local fsize
        fsize=$(du -h "$tb_file" 2>/dev/null | cut -f1)
        log_color "$YELLOW" "  ⚠ k=$k 文件存在但未完成 ($fsize)，重新求解..."
        rm -f "$tb_file"
    fi

    # 磁盘空间检查
    local needed=$((entries + 64))  # entries + header
    local avail
    avail=$(df --output=avail -B1 "$DATA_DIR" 2>/dev/null | tail -1)
    if [ -n "$avail" ] && [ "$avail" -lt "$needed" ]; then
        log_color "$RED" "  ✗ 磁盘空间不足！需要 $(format_size $needed)，可用 $(format_size $avail)"
        return 1
    fi

    local t_start
    t_start=$(date +%s)

    log "  条目数: $(printf "%'d" $entries)"
    log "  预计大小: $size_str"
    log "  求解中..."

    # 运行求解
    if "$SOLVE_ALL" \
        --data-dir "$DATA_DIR" \
        --threads "$THREADS" \
        --block-size "$BLOCK_SIZE" \
        --start-k "$k" \
        --end-k "$k" \
        --max-iters "$MAX_ITERS" \
        --verbose \
        2>&1 | tee -a "$LOG_FILE"; then

        local t_end elapsed fsize
        t_end=$(date +%s)
        elapsed=$((t_end - t_start))
        fsize=$(du -h "$tb_file" 2>/dev/null | cut -f1)

        log_color "$GREEN" "  ✓ k=$k 完成！耗时 $(format_seconds $elapsed)，文件大小 $fsize"

        # 记录到摘要
        echo "k=$k  OK  $(format_seconds $elapsed)  $fsize" >> "$SUMMARY_FILE"

        return 0
    else
        local t_end elapsed
        t_end=$(date +%s)
        elapsed=$((t_end - t_start))

        log_color "$RED" "  ✗ k=$k 失败！耗时 $(format_seconds $elapsed)"
        echo "k=$k  FAILED  $(format_seconds $elapsed)" >> "$SUMMARY_FILE"
        return 1
    fi
}

# ============================================================
# 求解所有桶
# ============================================================

solve_all_buckets() {
    banner "开始求解"
    log "  范围: k=$START_K → $END_K"
    log "  线程: $THREADS"
    log ""

    local total_start failed skipped
    total_start=$(date +%s)
    failed=0
    skipped=0

    for k in $(seq "$START_K" "$END_K"); do
        if is_bucket_done "$k"; then
            log_color "$CYAN" "  k=$k 已完成，跳过"
            skipped=$((skipped + 1))
            continue
        fi

        if ! solve_one_bucket "$k"; then
            failed=$((failed + 1))
            log_color "$RED" "  ✗ k=$k 求解失败，终止"
            break
        fi
    done

    local total_end total_elapsed
    total_end=$(date +%s)
    total_elapsed=$((total_end - total_start))

    banner "求解阶段结束"
    log "  总耗时: $(format_seconds $total_elapsed)"
    log "  跳过:   $skipped 桶（已完成）"
    log "  失败:   $failed 桶"

    if [ $failed -gt 0 ]; then
        return 1
    fi
    return 0
}

# ============================================================
# 导出开局库
# ============================================================

dump_book() {
    banner "导出开局库"

    local output="$SCRIPT_DIR/opening_book.json"

    if "$DUMP_BOOK" --data-dir "$DATA_DIR" --output "$output" 2>&1 | tee -a "$LOG_FILE"; then
        log_color "$GREEN" "✓ 开局库已导出: $output"
        log "  内容预览:"
        head -20 "$output" | while read -r line; do log "    $line"; done
    else
        log_color "$RED" "✗ 开局库导出失败"
    fi
}

# ============================================================
# 验证
# ============================================================

run_verify() {
    banner "验证"

    log "  对称性检查 + 终局一致性 (10000 样本)..."
    if "$VERIFY" --data-dir "$DATA_DIR" --samples 10000 2>&1 | tee -a "$LOG_FILE"; then
        log_color "$GREEN" "✓ 验证通过"
    else
        log_color "$RED" "✗ 验证失败"
    fi
}

# ============================================================
# 显示最终统计
# ============================================================

show_final_stats() {
    banner "最终统计"

    log "  表库文件:"
    local total_size=0
    for k in $(seq 4 15); do
        local tb_file="$DATA_DIR/wsf_tb_dtc_k$(printf "%02d" $k).bin"
        if [ -f "$tb_file" ]; then
            local fsize completed_str
            fsize=$(stat -c%s "$tb_file" 2>/dev/null || echo 0)
            total_size=$((total_size + fsize))
            fsize_h=$(du -h "$tb_file" 2>/dev/null | cut -f1)
            if is_bucket_done "$k"; then
                completed_str="${GREEN}✓${RESET}"
            else
                completed_str="${RED}✗${RESET}"
            fi
            log "    k=$(printf "%02d" $k): $fsize_h  $completed_str"
        fi
    done
    local total_h
    total_h=$(echo "scale=2; $total_size / 1073741824" | bc)
    log "  总大小: ${total_h} GB"

    echo ""
    log "  日志文件: $LOG_FILE"
    log "  摘要文件: $SUMMARY_FILE"
}

# ============================================================
# 主流程
# ============================================================

main() {
    # 初始化日志
    {
        echo "=== 狼羊棋硬解日志 ==="
        echo "开始时间: $(date)"
        echo "工作目录: $SCRIPT_DIR"
        echo ""
    } > "$LOG_FILE"

    # 初始化摘要
    {
        echo "# 狼羊棋硬解摘要 — $(date)"
        echo ""
    } > "$SUMMARY_FILE"

    banner "狼羊棋硬解 — 全量求解"
    log "开始时间: $(date '+%Y-%m-%d %H:%M:%S')"
    log "日志文件: $LOG_FILE"

    # 1. 环境检查
    check_env

    # 2. 规模预算
    print_budget

    if $DRY_RUN; then
        log_color "$YELLOW" "  --dry-run 模式，不执行求解"
        exit 0
    fi

    # 3. 编译
    build

    # 4. 求解
    if ! solve_all_buckets; then
        log_color "$RED" "求解未完全成功，跳过后续步骤"
        show_final_stats
        exit 1
    fi

    # 5. 导出开局库
    dump_book

    # 6. 验证
    run_verify

    # 7. 最终统计
    show_final_stats

    banner "全部完成！"
    log "完成时间: $(date '+%Y-%m-%d %H:%M:%S')"
    log ""
    log "下一步："
    log "  1. 查看开局库: cat opening_book.json"
    log "  2. 查看日志:   less $LOG_FILE"
    log "  3. 重新验证:   $VERIFY --data-dir $DATA_DIR --samples 100000"
}

main "$@"