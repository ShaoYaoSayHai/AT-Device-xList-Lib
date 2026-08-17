# ============================================================
# ADX (AT Device X) Makefile
# ============================================================
# 默认交叉编译（arm-none-eabi），可通过环境变量或命令行覆盖：
#   make                              用默认 arm-none-eabi-gcc 编译
#   make CC=gcc AR=ar                 切回主机 gcc 做语法验证
#   make clean                        清理 build/
# ============================================================

# ---- 工具链（默认 arm 交叉编译）----
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -std=c99 -O2 -g
ARFLAGS  = rcs

# ---- 路径 ----
SRC_DIR   := source
BUILD_DIR := build

# ---- 源文件 ----
# 注意：example/ 目录下的 usage_example.c 仅作示例（含 main/FreeRTOS/RT-Thread 演示），
#       依赖外部 RTOS 头文件，不纳入默认构建。
LIB_SRCS := $(SRC_DIR)/adx_at_engine.c \
            $(SRC_DIR)/adx_port.c

# ---- 对象文件 ----
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))

# ---- 头文件搜索路径 ----
INCLUDES := -I$(SRC_DIR)

# ---- 产物 ----
LIB     := $(BUILD_DIR)/libadx.a

# ============================================================
# 规则
# ============================================================

.PHONY: all lib clean

all: lib

# 静态库
lib: $(LIB)

$(LIB): $(LIB_OBJS) | $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $^

# ---- 编译规则 ----
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---- 目录 ----
# 注：使用 POSIX 命令，要求 make 后端为 sh
#     （Linux/macOS 天然满足；Windows 需 xpack-windows-build-tools / MSYS2 / Git Bash 提供 sh）
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ---- 清理 ----
clean:
	rm -rf $(BUILD_DIR)
