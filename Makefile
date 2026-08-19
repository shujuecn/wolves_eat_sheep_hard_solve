# Makefile for Wolves Eat Sheep Hard Solve
# Usage: make [target]

CXX := g++
CXXFLAGS := -std=c++20 -O3 -march=native -flto -Wall -Wextra
LDFLAGS := -flto -pthread

# 本地编译环境（从 .deb 包提取的 g++，已从 /tmp 迁移到持久路径）
# 使用 -idirafter 确保 libc 头文件在 C++ 头文件之后搜索
LOCAL_PREFIX := /home/agent074/tools/gcc-local
CXXFLAGS += -idirafter $(LOCAL_PREFIX)/usr/include
CXXFLAGS += -idirafter $(LOCAL_PREFIX)/usr/include/x86_64-linux-gnu
LDFLAGS += -L$(LOCAL_PREFIX)/usr/lib/x86_64-linux-gnu
LDFLAGS += -Wl,-rpath,$(LOCAL_PREFIX)/usr/lib/x86_64-linux-gnu

# 尝试使用 OpenMP
ifeq ($(shell echo | $(CXX) -fopenmp -x c++ - -o /dev/null 2>/dev/null && echo yes),yes)
    CXXFLAGS += -fopenmp
endif

BUILD_DIR := build
INCLUDE_DIR := include
SRC_DIR := src
TOOLS_DIR := tools
DATA_DIR := data/tb

# 源文件
BOARD_SRC := $(SRC_DIR)/board.cpp
ENCODE_SRC := $(SRC_DIR)/encode.cpp
SYMMETRY_SRC := $(SRC_DIR)/symmetry.cpp
TABLEBASE_SRC := $(SRC_DIR)/tablebase.cpp
SOLVER_SRC := $(SRC_DIR)/solver.cpp
SOLVER_RETRO_SRC := $(SRC_DIR)/solver_retro.cpp

# 目标文件
BOARD_OBJ := $(BUILD_DIR)/board.o
ENCODE_OBJ := $(BUILD_DIR)/encode.o
SYMMETRY_OBJ := $(BUILD_DIR)/symmetry.o
TABLEBASE_OBJ := $(BUILD_DIR)/tablebase.o
SOLVER_OBJ := $(BUILD_DIR)/solver.o
SOLVER_RETRO_OBJ := $(BUILD_DIR)/solver_retro.o

# 可执行文件
TARGETS := $(BUILD_DIR)/solve_all $(BUILD_DIR)/solve_retro \
           $(BUILD_DIR)/dump_opening_book \
           $(BUILD_DIR)/verify $(BUILD_DIR)/crosscheck $(BUILD_DIR)/selfcheck \
           $(BUILD_DIR)/check_tb $(BUILD_DIR)/stat_tb

.PHONY: all clean test solve solve-retro dump-book verify check-tb stat-tb web help

all: $(TARGETS)

# 创建目录
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(DATA_DIR):
	@mkdir -p $(DATA_DIR)

# 编译目标文件
$(BOARD_OBJ): $(BOARD_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(ENCODE_OBJ): $(ENCODE_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(SYMMETRY_OBJ): $(SYMMETRY_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(TABLEBASE_OBJ): $(TABLEBASE_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(SOLVER_OBJ): $(SOLVER_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(SOLVER_RETRO_OBJ): $(SOLVER_RETRO_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 链接可执行文件
$(BUILD_DIR)/crosscheck: $(TOOLS_DIR)/crosscheck.cpp $(BOARD_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< $(BOARD_OBJ) $(LDFLAGS) -o $@

$(BUILD_DIR)/selfcheck: $(TOOLS_DIR)/selfcheck.cpp $(BOARD_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< $(BOARD_OBJ) $(LDFLAGS) -o $@

$(BUILD_DIR)/solve_all: $(TOOLS_DIR)/solve_all.cpp \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/solve_retro: $(TOOLS_DIR)/solve_retro.cpp \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) $(SOLVER_RETRO_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) $(SOLVER_RETRO_OBJ) \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/dump_opening_book: $(TOOLS_DIR)/dump_opening_book.cpp \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/verify: $(TOOLS_DIR)/verify.cpp \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< \
		$(BOARD_OBJ) $(ENCODE_OBJ) $(SYMMETRY_OBJ) $(TABLEBASE_OBJ) $(SOLVER_OBJ) \
		$(LDFLAGS) -o $@

$(BUILD_DIR)/check_tb: $(TOOLS_DIR)/check_tb.cpp $(BOARD_OBJ) $(ENCODE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< $(BOARD_OBJ) $(ENCODE_OBJ) $(LDFLAGS) -o $@

$(BUILD_DIR)/stat_tb: $(TOOLS_DIR)/stat_tb.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< $(LDFLAGS) -o $@

# 测试
test: $(BUILD_DIR)/selfcheck $(BUILD_DIR)/crosscheck
	@echo "=== Running selfcheck ==="
	$(BUILD_DIR)/selfcheck
	@echo ""
	@echo "=== Running crosscheck (initial position) ==="
	@echo "sssss/sssss/sssss/5/1www1 w 0" | $(BUILD_DIR)/crosscheck

# 求解（原始算法）
solve: $(BUILD_DIR)/solve_all | $(DATA_DIR)
	$(BUILD_DIR)/solve_all --data-dir $(DATA_DIR) --verbose

# 求解（逆向分析算法）
solve-retro: $(BUILD_DIR)/solve_retro | $(DATA_DIR)
	$(BUILD_DIR)/solve_retro --data-dir $(DATA_DIR) --verbose

# 导出开局库
dump-book: $(BUILD_DIR)/dump_opening_book | $(DATA_DIR)
	$(BUILD_DIR)/dump_opening_book --data-dir $(DATA_DIR) --output opening_book.json

# 验证
verify: $(BUILD_DIR)/verify | $(DATA_DIR)
	$(BUILD_DIR)/verify --data-dir $(DATA_DIR) --samples 10000

# 独立校验 k=4/k=5 表库（逐局面 minimax 一致性）
check-tb: $(BUILD_DIR)/check_tb | $(DATA_DIR)
	$(BUILD_DIR)/check_tb --data-dir $(DATA_DIR) --threads $$(nproc)

# 表库结果分布统计（只读，自动跳过未完成桶）
stat-tb: $(BUILD_DIR)/stat_tb
	$(BUILD_DIR)/stat_tb --data-dir data/tb_test

# 网页版：可视化人机对战（ssh -L 端口转发后本机浏览器玩）
web:
	python3 web/server.py

# 清理
clean:
	rm -rf $(BUILD_DIR)

# 帮助
help:
	@echo "Wolves Eat Sheep — Hard Solve"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build all targets (default)"
	@echo "  test        Run selfcheck and crosscheck"
	@echo "  solve       Run the original iterative solve (data/tb)"
	@echo "  solve-retro Run the retrograde solve (data/tb)"
	@echo "  dump-book   Export opening book JSON"
	@echo "  verify      Verify tablebase consistency"
	@echo "  check-tb    Independent per-state minimax check of k=4/k=5"
	@echo "  stat-tb     Result distribution over completed buckets (--raw)"
	@echo "  web         Web human-vs-AI game (http://127.0.0.1:8080)"
	@echo "  clean       Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make -j8 solve    # Solve with 8 parallel jobs"
	@echo "  make test         # Run tests"