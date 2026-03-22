# Phase 0 · 环境与工具链

> **本章目标**：从零搭建 Cinux 开发环境，完成工具链验证，构建第一个可启动的 MBR 存根，跑通测试框架。
>
> **完成后的效果**：`make run` 启动 QEMU，串口输出确认启动成功；`make test` 跑通冒烟测试。

---

## 前言：为什么要先折腾环境？

说实话，搭建开发环境这件事真的很容易让人劝退。你在网上随便搜个 OS 开发教程，十有八九都会告诉你"先装个交叉编译器"，然后丢给你一堆链接和命令，照着敲完也不知道自己干了啥。等到后面编译报错、链接报错、QEMU 起不来的时候，你根本不知道问题出在哪，只能绝望地对着终端发呆。

笔者在折腾 Cinux 的时候也踩过不少坑。有的教程用 NASM，有的用 GRUB，还有的直接上 Docker 镜像 —— 这对于想理解底层原理的人来说简直是灾难。如果你跟我一样，想搞清楚从 MBR 到内核的每一步是怎么运作的，那必须从最基础的工具链开始，一点一点把环境搭起来。

这一章我们做的事情很 boring：装工具、写检查脚本、配置 CMake、写个只有三行指令的 MBR 存根，再跑个 `1+1=2` 的测试。但这些东西都是后面写 bootloader 和内核的基础，现在偷懒跳过去，后面一定会还回来。

好了，废话不多说，我们开始上号。

---

## 1️⃣ 概念精讲

### 1.1 为什么需要特殊的编译选项？

OS 开发和普通应用开发最大的区别在于：内核运行在没有操作系统的裸机上（freestanding 环境），不能依赖任何标准库。

**好消息**：你**不需要**安装专门的交叉编译工具链，系统自带的 GCC 完全可以编译内核！关键是要打正确的 flags，告诉 GCC "别瞎自作聪明，按我的规矩来"。

为什么有人会觉得需要"交叉编译工具链"？因为系统的 GCC 默认编译的是"托管代码"（hosted），它会链接 glibc，假设有操作系统存在。而内核代码是"独立的"（freestanding），必须在没有操作系统的情况下运行。但只要加上正确的编译选项，系统 GCC 就能胜任：

- `-ffreestanding`：告诉编译器"别假设有标准库"
- `-fno-exceptions`：禁用异常（内核自实现）
- `-fno-rtti`：禁用运行时类型信息（dynamic_cast 等特性）
- `-fno-stack-protector`：禁用栈保护（内核自己管栈）
- `-mno-red-zone`：禁用红区优化（中断处理需要）
- `-mcmodel=kernel`：使用内核代码模型（内核可以链接到更高的地址）
- `-std=c++23`：用现代 C++，但不用标准库

### 1.2 什么是 MBR？

MBR（Master Boot Record，主引导记录）是磁盘的第一个扇区（512 字节）。BIOS 启动时会把它读入内存的 `0x7C00` 地址，然后跳过去执行。

MBR 的结构非常简单：

```
Offset  Size    Description
------- ------- --------------------------------------------------
0x000   440     Boot code（启动代码）
0x1B8   4       Disk signature（磁盘签名）
0x1BC   2       (unused)
0x1BE   64      Partition table（分区表，4 个 16 字节项）
0x1FE   2       Boot signature（魔数 0xAA55）
```

最后两个字节必须是 `0xAA55`（小端序存储为 `55 AA`），否则 BIOS 会认为这不是一个可启动的磁盘。

这一章我们写的 MBR 存根非常简单：关中断、停机、无限循环。后面几章会在此基础上扩展成完整的 bootloader。

### 1.3 什么是 CMake 和为什么要用它？

CMake 是一个构建系统生成器，它能根据不同平台生成对应的 Makefile 或 Visual Studio 项目。对于 OS 开发来说，CMake 的好处是：

- 声明式：告诉它"我要什么"，而不是"怎么做"
- 跨平台：同一套配置可以在 Linux、macOS、Windows 上跑
- 可扩展：方便添加自定义目标（如 `make run` 直接启动 QEMU）
- 测试集成：内置 CTest 支持，方便跑单元测试

不用 CMake 的话，你得手写一大堆 Makefile，每次改文件路径都要改半天，维护起来非常痛苦。

### 1.4 QEMU 基础知识

QEMU 是一个开源的机器模拟器和虚拟机。在 OS 开发里，我们主要用它来：

- 快速测试：不需要每次都重启真机
- 调试方便：可以连接 GDB 单步调试
- 串口输出：`-serial stdio` 把串口重定向到终端，方便看日志

常用的 QEMU 参数：

```
-m 256M              # 分配 256MB 内存
-drive file=...,format=raw  # 指定磁盘镜像
-serial stdio        # 串口重定向到标准输入输出
-no-reboot           # panic 时不自动重启
-no-shutdown         # 关机后不退出 QEMU
-s -S                # -s: GDB 监听 1234 端口；-S: 启动时暂停
```

### 1.5 AT&T 汇编语法速查

Cinux 使用 GNU Assembler（GAS），采用 AT&T 语法。和 Intel 语法相比，主要区别：

| 特性 | AT&T | Intel |
|------|------|-------|
| 操作数顺序 | `mov src, dst` | `mov dst, src` |
| 寄存器前缀 | `%rax` | `rax` |
| 立即数前缀 | `$0x1000` | `0x1000` |
| 内存寻址 | `disp(base, index, scale)` | `[base + index*scale + disp]` |
| 操作数大小后缀 | `movq`, `movl`, `movw` | `mov`, `mov dword` |

示例：

```asm
# AT&T 语法
movq $0x1000, %rax        # 立即数 0x1000 -> RAX
movq 8(%rsp), %rdi        # [RSP+8] -> RDI
movq (%rsp, %rax, 4), %rdx  # [RSP+RAX*4] -> RDX

# 等价的 Intel 语法
mov rax, 1000h
mov rdi, [rsp+8]
mov rdx, [rsp+rax*4]
```

注意：AT&T 语法里 `%` 表示寄存器，`$` 表示立即数。忘记加前缀是新手最容易犯的错误。

---

## 2️⃣ 动手实现

### Step 1：安装开发工具

**目标**：在系统上安装 QEMU 和 CMake。

**为什么这样做**：这些工具是 OS 开发的基础，少了任何一个都跑不起来。

**操作**：

```bash
# Ubuntu/Debian 系统
sudo apt update
sudo apt install -y qemu-system-x86_64 cmake

# 验证安装
gcc --version
g++ --version
qemu-system-x86_64 --version
cmake --version
```

**解释**：
- `qemu-system-x86`：x86/x86_64 架构的 QEMU 模拟器
- `cmake`：构建系统生成器

**可能出错点**：

⚠️ 如果你看到 "E: Unable to locate package gcc-multilib"，可能是你的系统不是 Ubuntu/Debian，或者源列表有问题。试试 `sudo apt install gcc` 然后 `gcc -m32 --version` 看看是否支持 32 位编译。

**验证成功**：所有命令都能输出版本信息。

---

### Step 2：编写工具链检查脚本

**目标**：创建 `scripts/check_toolchain.sh`，自动检查所有必需工具是否已安装。

**文件**：`scripts/check_toolchain.sh`

```bash
#!/bin/bash
#
# scripts/check_toolchain.sh
# @brief 验证 Cinux 开发所需的工具链是否已安装
#

# 颜色输出辅助函数
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查命令是否存在
check_command() {
    if command -v "$1" &> /dev/null; then
        echo -e "${GREEN}[OK]${NC} $1 found: $(command -v $1)"
        return 0
    else
        echo -e "${RED}[FAIL]${NC} $1 not found"
        return 1
    fi
}

# 缺失工具计数
MISSING_COUNT=0

echo "=== Cinux Toolchain Check ==="
echo ""

# 检查 GCC
echo "Checking GCC..."
if gcc --version &> /dev/null; then
    GCC_VER=$(gcc --version | head -n1)
    echo -e "${GREEN}[OK]${NC} gcc: $GCC_VER"
else
    echo -e "${RED}[FAIL]${NC} gcc not found"
    ((MISSING_COUNT++))
fi

# 检查 G++
echo "Checking G++..."
if g++ --version &> /dev/null; then
    GPP_VER=$(g++ --version | head -n1)
    echo -e "${GREEN}[OK]${NC} g++: $GPP_VER"
else
    echo -e "${RED}[FAIL]${NC} g++ not found"
    ((MISSING_COUNT++))
fi

# 检查 binutils
echo "Checking binutils..."
for tool in as ld objcopy; do
    if ! check_command $tool; then
        ((MISSING_COUNT++))
    fi
done

# 检查 QEMU
echo "Checking QEMU..."
if qemu-system-x86_64 --version &> /dev/null; then
    QEMU_VER=$(qemu-system-x86_64 --version | head -n1)
    echo -e "${GREEN}[OK]${NC} qemu-system-x86_64: $QEMU_VER"
else
    echo -e "${YELLOW}[WARN]${NC} qemu-system-x86_64 not found (optional for host tests)"
fi

# 检查 CMake
echo "Checking CMake..."
if cmake --version &> /dev/null; then
    CMAKE_VER=$(cmake --version | head -n1)
    CMAKE_MAJOR=$(cmake --version | head -n1 | grep -oP '\d+\.\d+' | head -n1 | cut -d. -f1)
    CMAKE_MINOR=$(cmake --version | head -n1 | grep -oP '\d+\.\d+' | head -n1 | cut -d. -f2)
    if [ "$CMAKE_MAJOR" -gt 3 ] || ([ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -ge 20 ]); then
        echo -e "${GREEN}[OK]${NC} cmake: $CMAKE_VER (>= 3.20)"
    else
        echo -e "${YELLOW}[WARN]${NC} cmake: $CMAKE_VER (recommended >= 3.20)"
    fi
else
    echo -e "${RED}[FAIL]${NC} cmake not found"
    ((MISSING_COUNT++))
fi

echo ""
echo "=== Summary ==="

if [ $MISSING_COUNT -eq 0 ]; then
    echo -e "${GREEN}All required tools found!${NC}"
    exit 0
else
    echo -e "${RED}Missing $MISSING_COUNT tool(s).${NC}"
    echo ""
    echo "To install missing tools on Ubuntu/Debian:"
    echo "  sudo apt install -y gcc-multilib g++-multilib binutils qemu-system-x86 cmake"
    exit 1
fi
```

**解释**：

- 使用 `command -v` 检测命令是否存在（比 `which` 更可靠）
- 用颜色区分 OK/FAIL/WARN 状态，方便肉眼快速识别
- 对 CMake 版本进行检查（需要 3.20+）
- 缺失工具时给出安装建议

**可能出错点**：

⚠️ 记得给脚本添加执行权限：`chmod +x scripts/check_toolchain.sh`，否则会报 "Permission denied"。

**验证**：

```bash
chmod +x scripts/check_toolchain.sh
./scripts/check_toolchain.sh
```

应该看到绿色的 `[OK]` 消息。

---

### Step 3：配置 CMake 编译选项

**目标**：创建 `cmake/toolchain-x86_64-elf.cmake`，用系统自带的 GCC 配置正确的编译选项。

**关键点**：我们**不用**专门的交叉编译器，直接用系统的 `gcc`/`g++`，只要把 flags 打对就行。

**文件**：`cmake/toolchain-x86_64-elf.cmake`

```cmake
# =============================================================================
# cmake/toolchain-x86_64-elf.cmake
# @brief Cinux 内核编译配置
#
# 注意：这里用的是系统自带的 gcc/g++，不是什么交叉编译器！
# 关键是把编译选项配置正确，让编译器知道我们在写 freestanding 代码。
# 用法: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake ..
# =============================================================================

# 目标系统名称为 Generic（告诉 CMake 这是 freestanding 环境）
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ============================================================
# 编译器配置：直接用系统的 gcc/g++
# ============================================================

find_program(CMAKE_C_COMPILER NAMES gcc gcc-11 gcc-12 gcc-13 gcc-14)
find_program(CMAKE_CXX_COMPILER NAMES g++ g++-11 g++-12 g++-13 g++-14)
find_program(CMAKE_ASM_COMPILER NAMES as)

# ============================================================
# 链接器配置：用系统的 ld
# ============================================================

set(CMAKE_LINKER ld)

# ============================================================
# 全局编译选项：这是核心！
# ============================================================

# C 编译标志
set(CMAKE_C_FLAGS_INIT "
    -ffreestanding           # 告诉编译器：别假设有标准库
    -fno-stack-protector     # 禁用栈保护（内核自己管栈）
    -mno-red-zone            # 禁用红区优化（中断处理需要）
    -mcmodel=kernel          # 内核代码模型（代码可以链接到高地址）
    -Wall                    # 启用所有警告
    -Wextra                  # 启用额外警告
")

# C++ 编译标志（继承 C 标志，加上 C++ 特定选项）
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}
    -fno-exceptions          # 禁用异常（内核自己实现异常机制）
    -fno-rtti                # 禁用运行时类型信息（不用 dynamic_cast）
    -std=c++23               # 用现代 C++，但不用标准库
")

# ASM 编译标志
set(CMAKE_ASM_FLAGS_INIT "-Wa,--divide")  # 允许汇编中用 / 作为除号

# 链接器标志
set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib                # 不链接标准库（libc、libstdc++ 等）
    -static                  # 静态链接
    -Wl,--build-id=none      # 禁用 build-id（减少二进制大小）
")

# ============================================================
# 查找路径配置
# ============================================================

set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

**解释**：

- **`CMAKE_SYSTEM_NAME=Generic`**：这是 CMake 的"freestanding 模式"开关，关掉各种托管环境的假设
- **`find_program`**：直接在系统里找 `gcc`/`g++`，不是什么 `x86_64-elf-gcc` 之类的东西
- **编译标志才是重点**：`-ffreestanding`、`-nostdlib` 这些 flags 告诉编译器"别瞎自作聪明"
- **`-Wa,--divide`**：汇编默认把 `/` 当注释符，这个选项允许用它做除号

**可能出错点**：

⚠️ 如果你的系统 GCC 版本太老（< 11），可能不支持 `-std=c++23`。可以改成 `-std=c++20` 或升级 GCC：

```bash
# Ubuntu 22.04 及以上默认 GCC 都支持 C++23
gcc --version
```

**验证**：暂时无法直接验证，等 CMakeLists.txt 写好后一起测试。

---

### Step 4：编写顶层 CMakeLists.txt

**目标**：创建 `CMakeLists.txt`，设置项目基本信息和构建流程。

**文件**：`CMakeLists.txt`

```cmake
# =============================================================================
# CMakeLists.txt
# @brief Cinux 操作系统顶层构建配置
# =============================================================================

# 设置 CMake 最低版本要求
cmake_minimum_required(VERSION 3.20)

# 声明项目
project(cinux
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

# ============================================================
# 工具链配置
# ============================================================

# 如果未指定工具链文件，使用项目自带的
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64-elf.cmake"
        CACHE FILEPATH "Toolchain file")
    message(STATUS "Using default toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# ============================================================
# 全局编译选项
# ============================================================

# 警告等级
add_compile_options(
    -Wall
    -Wextra
    -Wno-error=unused-parameter  # 参数未使用只警告不报错
)

# Debug/Release 配置
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# ============================================================
# 子目录配置
# ============================================================

# 添加 boot 子目录
add_subdirectory(boot)

# 添加 kernel 子目录
add_subdirectory(kernel)

# 添加 test 子目录
option(CINUX_BUILD_TESTS "Build host-side tests" ON)
if(CINUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ============================================================
# 包含 CMake 模块
# ============================================================

# 包含 QEMU 运行配置
include(cmake/qemu.cmake)

# ============================================================
# 输出配置信息
# ============================================================

message(STATUS "")
message(STATUS "=== Cinux Build Configuration ===")
message(STATUS "  Project:     ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler:  ${CMAKE_C_COMPILER}")
message(STATUS "  CXX Compiler:${CMAKE_CXX_COMPILER}")
message(STATUS "  ASM Compiler:${CMAKE_ASM_COMPILER}")
message(STATUS "  Toolchain:   ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  Build tests: ${CINUX_BUILD_TESTS}")
message(STATUS "===================================")
message(STATUS "")
```

**解释**：

- `project()` 声明项目名称、版本和支持的语言
- `add_subdirectory()` 递归处理子目录的 CMakeLists.txt
- `option()` 允许用户控制是否编译测试（`cmake -DCINUX_BUILD_TESTS=OFF`）
- 最后用 `message(STATUS)` 打印配置摘要，方便确认

**可能出错点**：

⚠️ CMake 对空格敏感。在 `if()` 语句里，条件前后必须有空格：`if(NOT XXX)` 而不是 `if(NOTXXX)`。

---

### Step 5：配置 QEMU 运行目标

**目标**：创建 `cmake/qemu.cmake`，定义 `make run` 和 `make run-debug` 目标。

**文件**：`cmake/qemu.cmake`

```cmake
# =============================================================================
# cmake/qemu.cmake
# @brief QEMU 运行配置
# =============================================================================

# 查找 QEMU 可执行文件
find_program(QEMU_EXECUTABLE qemu-system-x86_64)

if(NOT QEMU_EXECUTABLE)
    set(QEMU_EXECUTABLE "qemu-system-x86_64")
    message(WARNING "qemu-system-x86_64 not found in PATH, using default name")
endif()

# ============================================================
# QEMU 命令行参数配置
# ============================================================

# 基础 QEMU 参数
set(QEMU_COMMON_FLAGS "
    -m 256M                 # 分配 256MB 内存
    -serial stdio           # 串口重定向到标准输入输出
    -no-reboot              # panic 时不自动重启
    -no-shutdown            # 关机后不退出 QEMU
")

# 调试模式额外参数
set(QEMU_DEBUG_FLAGS "
    -s                      # 等同于 -gdb tcp::1234
    -S                      # 启动时暂停 CPU，等待 GDB 连接
")

# 磁盘镜像路径
set(CINUX_IMAGE_PATH "${CMAKE_BINARY_DIR}/cinux.img" CACHE PATH "Cinux disk image path")

# ============================================================
# 创建 MBR 目标（依赖 boot/mbr）
# ============================================================

# 确保 MBR 已编译
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh ${CINUX_IMAGE_PATH}
    DEPENDS mbr
    COMMENT "Building disk image: ${CINUX_IMAGE_PATH}"
    VERBATIM
)

# 镜像目标
add_custom_target(image ALL
    DEPENDS ${CINUX_IMAGE_PATH}
)

# ============================================================
# 定义 custom target: run
# ============================================================

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)

# ============================================================
# 定义 custom target: run-debug
# ============================================================

add_custom_target(run-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU in debug mode (GDB on :1234)"
    VERBATIM
)

# ============================================================
# 用户提示信息
# ============================================================

message(STATUS "QEMU targets:")
message(STATUS "  make run        : Start QEMU normally")
message(STATUS "  make run-debug  : Start QEMU with GDB server on :1234")
message(STATUS "  make image      : Build disk image only")
```

**解释**：

- `add_custom_command()` 创建自定义构建命令（生成镜像）
- `add_custom_target()` 创建 Make 目标（`make run` 就是调这个）
- `VERBATIM` 确保参数正确传递（避免转义问题）
- `ALL` 参数表示这个目标默认构建（`make` 时自动执行）

**可能出错点**：

⚠️ QEMU 参数里的空格和换行需要小心。如果你复制后看到奇怪的报错，检查一下是不是多了/少了引号。

**验证**：等所有文件写好后，运行 `cmake ..` 应该能看到 QEMU 相关的消息。

---

### Step 6：编写 MBR 存根

**目标**：创建 `boot/mbr_stub.S`，实现最小的 MBR。

**文件**：`boot/mbr_stub.S`

```asm
/**
 * @file boot/mbr_stub.S
 * @brief Cinux 最小 MBR（主引导记录）存根
 *
 * 这是 Cinux 项目最初期的 MBR 实现，用于验证构建链和 QEMU 启动。
 * 当前版本只做三件事：关中断、停机、等待。
 *
 * AT&T 语法说明：
 *   - 操作数顺序：源操作数, 目标操作数（与 Intel 语法相反）
 *   - 寄存器前缀：%
 *   - 立即数前缀：$
 *   - 内存寻址：offset(base, index, scale)
 *   - 注释格式：# 注释内容
 */

// ============================================================
// 代码段：16 位实模式代码
// ============================================================

.section .text
.code16                      // 告诉汇编器生成 16 位代码
.global _start                // 使 _start 对链接器可见

# ============================================================
# 函数名：_start
# 职责：MBR 入口点，BIOS 加载后从此处开始执行
# 输入：CS:IP = 0x0000:0x7C00（BIOS 将 MBR 加载到 0x7C00）
# 输出：无
# 影响：CLI 禁用中断，HLT 停止 CPU
# ============================================================
_start:

    # 步骤 1：关中断
    # 禁用中断，防止任何中断在初始化完成前触发
    cli                      # [->flags] 清除 IF 位，禁用可屏蔽中断

    # 步骤 2：停机等待
    # 停止 CPU 执行，直到收到中断（已禁用，所以实际上会永久停机）
    hlt                      # [->cpu] 停止指令执行

    # 步骤 3：防止继续执行
    # 如果某种情况下 HLT 被唤醒，跳回自己继续停机
    jmp _start               # [->IP] 无限循环，确保不会执行后面的数据

// ============================================================
// 数据段和填充
// ============================================================

# 用 0 填充到偏移 510（确保 MBR 正好 512 字节）
.fill 510 - (. - _start), 1, 0

# MBR 签名（魔数）
# BIOS 会检查最后两个字节是否为 0xAA55，否则视为无效 MBR
.word 0xAA55                 # [->memory:510] 写入小端序签名 0xAA55
```

**解释**：

- `.code16`：告诉 GAS 生成 16 位代码（MBR 在实模式下运行）
- `cli`：Clear Interrupts，禁用可屏蔽中断
- `hlt`：Halt，停止 CPU 执行
- `.fill`：填充字节，确保 MBR 正好 510 字节（不含签名）
- `.word 0xAA55`：写入 MBR 签名（注意 GAS 的 `.word` 是小端序）

**可能出错点**：

⚠️ MBR 签名必须是 `0xAA55`，但 `.word` 写入的是小端序，所以磁盘上实际是 `55 AA`。BIOS 读取后会当成 `AA55`，这是对的。别搞反了。

**验证**：等构建系统完成后，运行 `make run`，QEMU 应该会启动（虽然屏幕是黑的，因为没有输出）。

---

### Step 7：编写 boot/CMakeLists.txt

**目标**：创建 `boot/CMakeLists.txt`，配置 MBR 编译。

**文件**：`boot/CMakeLists.txt`

```cmake
# =============================================================================
# boot/CMakeLists.txt
# @brief Bootloader 模块构建配置
# =============================================================================

# ============================================================
# MBR 目标
# ============================================================

# 创建 MBR 可执行文件（ELF 格式）
add_executable(mbr
    mbr_stub.S
)

# 设置 MBR 链接选项：生成原始二进制
target_link_options(mbr PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/mbr.ld    # 使用自定义链接脚本
    -nostdlib                                 # 不链接标准库
)

# 链接后处理：转换为纯二进制格式
add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
    VERBATIM
)

# ============================================================
# 链接器脚本
# ============================================================

# 创建 MBR 链接脚本
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf64-x86-64\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : { *(.text) }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")
```

**解释**：

- `-T mbr.ld`：使用自定义链接脚本，将代码放在 `0x7C00`
- `objcopy -O binary`：将 ELF 转为纯二进制（MBR 必须是裸二进制）
- 链接脚本用 `file(WRITE ...)` 内联生成，避免额外文件

**可能出错点**：

⚠️ 如果 `objcopy` 报 "File format not recognized"，说明 `add_executable` 生成了错误格式的文件。检查工具链配置。

---

### Step 8：编写镜像构建脚本

**目标**：创建 `scripts/build_image.sh`，将 MBR 写入磁盘镜像。

**文件**：`scripts/build_image.sh`

```bash
#!/bin/bash
#
# scripts/build_image.sh
# @brief 构建并生成 Cinux 磁盘镜像
#

set -e  # 遇到错误立即退出

# ============================================================
# 路径配置
# ============================================================

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
BUILD_DIR=${PROJECT_ROOT}/build
OUTPUT_IMAGE=${1:-${BUILD_DIR}/cinux.img}

# ============================================================
# 确保构建目录存在
# ============================================================

mkdir -p "$BUILD_DIR"

# ============================================================
# 源文件路径配置
# ============================================================

MBR_BIN=${BUILD_DIR}/boot/mbr.bin

# 检查 MBR 是否存在
if [ ! -f "$MBR_BIN" ]; then
    echo "Error: MBR binary not found at $MBR_BIN"
    echo "Please run 'make' first to build the bootloader."
    exit 1
fi

# ============================================================
# 创建磁盘镜像
# ============================================================

# 步骤 1：创建空白镜像（1MB）
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# 步骤 2：写入 MBR
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none

# ============================================================
# 验证镜像
# ============================================================

# 验证 MBR 签名
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    echo "MBR signature valid: 0xAA55"
else
    echo "Warning: MBR signature invalid: $SIGNATURE (expected 55aa)"
fi

# ============================================================
# 输出结果信息
# ============================================================

SIZE=$(stat -c%s "$OUTPUT_IMAGE" 2>/dev/null || stat -f%z "$OUTPUT_IMAGE")
echo "Disk image built successfully!"
echo "  Path: $OUTPUT_IMAGE"
echo "  Size: $SIZE bytes"
echo ""
echo "To run Cinux:"
echo "  make run    # or"
echo "  qemu-system-x86_64 -drive file=$OUTPUT_IMAGE,format=raw -serial stdio"
```

**解释**：

- `set -e`：任何命令失败就退出（避免继续执行产生错误结果）
- `dd if=/dev/zero ...`：创建空白镜像
- `conv=notrunc`：不截断输出文件（保留镜像大小）
- `xxd -p`：以十六进制打印，用于验证签名

**可能出错点**：

⚠️ macOS 的 `stat` 命令参数不同，脚本里做了兼容处理。如果还有问题，手动检查 `ls -l build/cinux.img`。

**验证**：

```bash
chmod +x scripts/build_image.sh
# 等构建系统完成后运行
./scripts/build_image.sh
xxd build/cinux.img | tail -n 3
```

应该看到最后两行是 `00510: 0055 aa`（签名）。

---

### Step 9：编写测试框架和冒烟测试

**目标**：创建 `test/framework/test_framework.h` 和 `test/unit/test_smoke.cpp`，实现基础测试框架。

**文件**：`test/framework/test_framework.h`

```cpp
/**
 * @file test_framework.h
 * @brief Cinux 自研轻量测试框架
 *
 * 支持两种运行模式：
 *   - Host 模式（-DCINUX_HOST_TEST）：g++ 编译，在 Linux 上运行
 *   - QEMU 模式：直接在内核中运行，通过串口输出结果
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// ============================================================
// 平台适配层
// ============================================================

#ifdef CINUX_HOST_TEST
    #include <stdio.h>
    #include <stdlib.h>
    #define _TEST_PRINT(fmt, ...)   printf(fmt, ##__VA_ARGS__)
    #define _TEST_ABORT()           abort()
#else
    // QEMU 模式：依赖内核串口驱动
    extern void serial_printf(const char* fmt, ...);
    #define _TEST_PRINT(fmt, ...)   serial_printf(fmt, ##__VA_ARGS__)
    #define _TEST_ABORT()           do { asm volatile("hlt"); } while(1)
#endif

// ============================================================
// 内部测试注册机制
// ============================================================

typedef void (*_test_fn_t)(void);

struct _TestEntry {
    const char*  name;
    const char*  file;
    int          line;
    _test_fn_t   fn;
};

#define _MAX_TESTS 256

#ifdef CINUX_HOST_TEST
    extern _TestEntry _test_registry[_MAX_TESTS];
    extern int        _test_count;

    static inline void _register_test(const char* name, const char* file,
                                       int line, _test_fn_t fn) {
        if (_test_count < _MAX_TESTS) {
            _test_registry[_test_count++] = {name, file, line, fn};
        }
    }
#endif

// ============================================================
// 统计
// ============================================================

extern int _tests_passed;
extern int _tests_failed;

// ============================================================
// TEST() 宏
// ============================================================

#define TEST(test_name)                                                 \
    static void _test_fn_##__LINE__();                                  \
    static struct _TestAutoReg_##__LINE__ {                             \
        _TestAutoReg_##__LINE__() {                                     \
            _register_test(test_name, __FILE__, __LINE__,               \
                           _test_fn_##__LINE__);                        \
        }                                                               \
    } _test_reg_##__LINE__;                                             \
    static void _test_fn_##__LINE__()

// ============================================================
// ASSERT_* 宏
// ============================================================

#define ASSERT_TRUE(expr)                                               \
    do {                                                                \
        if (!(expr)) {                                                  \
            _TEST_PRINT("[FAIL] %s\n  ASSERT_TRUE(%s) failed\n"        \
                        "  at %s:%d\n", _current_test_name,            \
                        #expr, __FILE__, __LINE__);                     \
            _tests_failed++;                                            \
            return;                                                     \
        }                                                               \
    } while(0)

#define ASSERT_FALSE(expr)   ASSERT_TRUE(!(expr))

#define ASSERT_EQ(actual, expected)                                     \
    do {                                                                \
        auto _a = (actual);                                             \
        auto _e = (expected);                                           \
        if (!(_a == _e)) {                                              \
            _TEST_PRINT("[FAIL] %s\n  ASSERT_EQ(%s, %s) failed\n"      \
                        "  at %s:%d\n", _current_test_name,            \
                        #actual, #expected, __FILE__, __LINE__);        \
            _tests_failed++;                                            \
            return;                                                     \
        }                                                               \
    } while(0)

#define ASSERT_NE(actual, expected)                                     \
    do {                                                                \
        auto _a = (actual);                                             \
        auto _e = (expected);                                           \
        if (_a == _e) {                                                 \
            _TEST_PRINT("[FAIL] %s\n  ASSERT_NE(%s, %s) failed\n"      \
                        "  at %s:%d\n", _current_test_name,            \
                        #actual, #expected, __FILE__, __LINE__);        \
            _tests_failed++;                                            \
            return;                                                     \
        }                                                               \
    } while(0)

#define ASSERT_NULL(ptr)                                                \
    ASSERT_TRUE((ptr) == nullptr)

#define ASSERT_NOT_NULL(ptr)                                            \
    ASSERT_TRUE((ptr) != nullptr)

#define ASSERT_GE(a, b)  ASSERT_TRUE((a) >= (b))
#define ASSERT_LE(a, b)  ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b)  ASSERT_TRUE((a) >  (b))
#define ASSERT_LT(a, b)  ASSERT_TRUE((a) <  (b))

// ============================================================
// 运行所有测试
// ============================================================

extern const char* _current_test_name;

static inline void RUN_ALL_TESTS() {
    extern _TestEntry _test_registry[];
    extern int        _test_count;

    _TEST_PRINT("\n=== Cinux Test Runner ===\n");
    _TEST_PRINT("Running %d test(s)...\n\n", _test_count);

    _tests_passed = 0;
    _tests_failed = 0;

    for (int i = 0; i < _test_count; i++) {
        _current_test_name = _test_registry[i].name;
        _test_registry[i].fn();
        if (_tests_failed == 0) {
            _TEST_PRINT("[PASS] %s\n", _current_test_name);
            _tests_passed++;
        }
    }

    _TEST_PRINT("\n=== Results: %d passed, %d failed ===\n",
                _tests_passed, _tests_failed);

    if (_tests_failed > 0) {
        _TEST_PRINT("[SUITE FAILED]\n");
    } else {
        _TEST_PRINT("[SUITE PASSED]\n");
    }
}

// ============================================================
// 全局变量定义
// ============================================================

#ifdef TEST_FRAMEWORK_IMPL
    _TestEntry  _test_registry[_MAX_TESTS];
    int         _test_count   = 0;
    int         _tests_passed = 0;
    int         _tests_failed = 0;
    const char* _current_test_name = "";
#endif
```

**文件**：`test/unit/test_smoke.cpp`

```cpp
/**
 * @file test/unit/test_smoke.cpp
 * @brief Cinux 测试框架冒烟测试
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

// ============================================================
// 正常路径测试
// ============================================================

TEST("smoke: 1+1=2") {
    int a = 1;
    int b = 1;
    int expected = 2;

    int result = a + b;

    ASSERT_EQ(result, expected);
    ASSERT_TRUE(result == expected);
    ASSERT_FALSE(result != expected);
    ASSERT_GT(result, 1);      // result > 1
    ASSERT_GE(result, 2);      // result >= 2
    ASSERT_LE(result, 2);      // result <= 2
    ASSERT_LT(result, 3);      // result < 3
}

// ============================================================
// 边界条件测试
// ============================================================

TEST("smoke: boundary values") {
    int zero = 0;
    ASSERT_EQ(zero, 0);
    ASSERT_TRUE(zero == 0);
    ASSERT_FALSE(zero != 0);
    ASSERT_GE(zero, 0);
    ASSERT_LE(zero, 0);

    int negative = -1;
    ASSERT_EQ(negative, -1);
    ASSERT_LT(negative, 0);
    ASSERT_TRUE(negative < 0);

    ASSERT_EQ(negative, negative);
    ASSERT_NE(negative, zero);
}

// ============================================================
// 指针测试
// ============================================================

TEST("smoke: pointer assertions") {
    int* null_ptr = nullptr;
    ASSERT_NULL(null_ptr);
    ASSERT_EQ(null_ptr, nullptr);

    int value = 42;
    int* valid_ptr = &value;
    ASSERT_NOT_NULL(valid_ptr);
    ASSERT_NE(valid_ptr, nullptr);

    ASSERT_EQ(*valid_ptr, 42);
}

// ============================================================
// 主函数
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
```

**文件**：`test/CMakeLists.txt`

```cmake
# =============================================================================
# test/CMakeLists.txt
# @brief Cinux 测试配置
# =============================================================================

# ============================================================
# 测试编译选项
# ============================================================

add_compile_definitions(CINUX_HOST_TEST)

# ============================================================
# 测试可执行文件：test_smoke
# ============================================================

add_executable(test_smoke
    unit/test_smoke.cpp
)

target_include_directories(test_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
)

# ============================================================
# CTest 配置
# ============================================================

add_test(NAME smoke COMMAND test_smoke)

# ============================================================
# 测试辅助目标
# ============================================================

add_custom_target(test
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke
    COMMENT "Running tests..."
    VERBATIM
)
```

**解释**：

- 测试框架用 `TEST()` 宏自动注册测试用例
- `ASSERT_*` 宏在失败时打印文件名和行号
- Host 模式用 `printf`，QEMU 模式用 `serial_printf`（后面实现）

**可能出错点**：

⚠️ 如果编译时看到 `error: 'nullptr' was not declared`，说明没有包含 `<stddef.h>` 或编译器太老。加上 `#include <stddef.h>` 或升级 GCC。

**验证**：

```bash
cd build
make test_smoke
./test/unit/test_smoke
```

应该看到绿色的 `[PASS]` 消息。

---

### Step 10：创建 kernel 占位文件

**目标**：创建 `kernel/CMakeLists.txt`，为后续内核开发预留位置。

**文件**：`kernel/CMakeLists.txt`

```cmake
# =============================================================================
# kernel/CMakeLists.txt
# @brief Cinux 内核模块 CMake 配置
#
# 本 Milestone 中仅为占位，实际的内核源文件将在后续 Milestone 添加。
# =============================================================================

# 创建接口库用于传递编译选项
add_library(cinux_kernel INTERFACE)

# 设置内核头文件目录
target_include_directories(cinux_kernel INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 后续 Milestone 会添加：
#   - 内核源文件（boot.S, crt_stub.cpp, kprintf.cpp, etc.）
#   - 链接器脚本（linker.ld）
#   - 内核依赖
```

---

## 3️⃣ 构建与运行

好了，所有文件都准备好了。现在我们试跑一下，看看能不能成功构建。

### 首次构建

```bash
# 1. 创建构建目录
mkdir build
cd build

# 2. 配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 3. 编译
make

# 4. 运行测试
make test

# 5. 启动 QEMU
make run
```

**预期输出**：

```
=== Cinux Build Configuration ===
  Project:     cinux v0.1.0
  Build type:  Debug
  C Compiler:  /usr/bin/gcc
  CXX Compiler:/usr/bin/g++
  ASM Compiler:/usr/bin/as
  Toolchain:   /path/to/cmake/toolchain-x86_64-elf.cmake
  Build tests: TRUE
===================================

QEMU targets:
  make run        : Start QEMU normally
  make run-debug  : Start QEMU with GDB server on :1234
  make image      : Build disk image only
```

**测试输出**：

```
=== Cinux Test Runner ===
Running 3 test(s)...

[PASS] smoke: 1+1=2
[PASS] smoke: boundary values
[PASS] smoke: pointer assertions

=== Results: 3 passed, 0 failed ===
[SUITE PASSED]
```

**QEMU 启动**：

QEMU 会启动一个窗口（黑的，因为没有 VGA 输出），终端会保持连接状态。因为我们的 MBR 只是 `cli; hlt; jmp`，所以看起来像"卡死"了，这实际上是正常的。

按 `Ctrl+A` 然后按 `X` 退出 QEMU。

### 验证 MBR 签名

```bash
xxd build/cinux.img | tail -n 3
```

应该看到：

```
0000050: 0000 0000 0000 0000 0000 0000 0000 0000  ................
0000060: 0000 0000 0000 0000 0000 0000 0000 0000  ................
0000070: 0000 0000 0000 0000 0000 0000 0000 55aa  ..............U.
```

最后两个字节是 `55 aa`（小端序的 `0xAA55`），说明 MBR 签名正确。

---

## 4️⃣ 调试技巧

### 常见问题排查

#### 问题 1：CMake 找不到编译器

**错误信息**：

```
CMake Error at CMakeLists.txt:17 (project):
  No CMAKE_C_COMPILER could be found
```

**解决方法**：

```bash
# 检查 GCC 是否安装
gcc --version

# 如果没装，安装它
sudo apt install gcc g++

# 或者手动指定编译器
cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ ..
```

#### 问题 2：QEMU 起不来，提示 "Could not open ..."

**错误信息**：

```
qemu-system-x86_64: -drive file=build/cinux.img,format=raw: could not open disk image ...
```

**解决方法**：

```bash
# 检查镜像是否存在
ls -l build/cinux.img

# 如果不存在，手动构建
cd build
make image
```

#### 问题 3：MBR 签名错误

**错误信息**：

```
Warning: MBR signature invalid: 0000 (expected 55aa)
```

**解决方法**：

检查 `boot/mbr_stub.S` 的最后两行：

```asm
.fill 510 - (. - _start), 1, 0
.word 0xAA55
```

确保 `.word` 的值是 `0xAA55` 而不是 `0x55AA`（GAS 会自动处理小端序）。

### GDB 调试 QEMU

如果需要调试 MBR，可以用 GDB 连接到 QEMU：

```bash
# 终端 1：启动 QEMU 调试模式
cd build
make run-debug

# 终端 2：连接 GDB
gdb build/boot/mbr
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
```

**注意**：当前 MBR 是 16 位代码，GDB 可能无法正常单步执行。需要用 `si`（stepi）指令而不是 `s`（step）。

### 查看串口输出

将串口输出保存到文件：

```bash
qemu-system-x86_64 \
    -drive file=build/cinux.img,format=raw \
    -serial file:serial.log \
    -no-reboot -no-shutdown

# 另开终端查看
tail -f serial.log
```

---

## 5️⃣ 本章小结

### 新增关键文件

| 文件 | 作用 | 关键内容 |
|------|------|----------|
| `scripts/check_toolchain.sh` | 工具链检查脚本 | 验证 GCC/G++/QEMU/CMake |
| `cmake/toolchain-x86_64-elf.cmake` | GCC 编译选项配置 | 使用系统 GCC，设置 freestanding 编译标志 |
| `CMakeLists.txt` | 顶层构建配置 | 引入工具链，添加子目录 |
| `cmake/qemu.cmake` | QEMU 运行配置 | 定义 run/run-debug 目标 |
| `boot/mbr_stub.S` | MBR 存根 | cli/hlt/jmp 循环 |
| `scripts/build_image.sh` | 镜像构建脚本 | dd 写入 MBR，验证签名 |
| `test/framework/test_framework.h` | 测试框架 | TEST/ASSERT_* 宏 |
| `test/unit/test_smoke.cpp` | 冒烟测试 | 1+1=2 等基础测试 |
| `test/CMakeLists.txt` | 测试配置 | HOST_TEST 模式，CTest 集成 |
| `kernel/CMakeLists.txt` | 内核占位 | 后续扩展 |

### 新增 Make 目标

| 目标 | 效果 |
|------|------|
| `make` | 构建所有目标（MBR、测试） |
| `make image` | 生成磁盘镜像 |
| `make run` | 启动 QEMU |
| `make run-debug` | 启动 QEMU + GDB server |
| `make test` | 运行测试套件 |

### 下一章预告

下一章（`001_boot_real_mode`）我们会：

1. 在 MBR 中添加屏幕输出（通过 BIOS INT 10h）
2. 实现磁盘读取（通过 BIOS INT 13h 扩展读取）
3. 编写 Stage2 bootloader
4. 在保护模式下初始化串口
5. 完成实模式到保护模式的切换

完成本章后，我们会有一个可以在 QEMU 中显示文字的 bootloader，为后续进入长模式打下基础。

---

## 附录：完整文件清单

```
cinux/
├── CMakeLists.txt                      # 顶层构建配置
├── cmake/
│   ├── qemu.cmake                      # QEMU 运行目标
│   └── toolchain-x86_64-elf.cmake      # GCC 编译选项配置
├── boot/
│   ├── CMakeLists.txt                  # Boot 模块配置
│   └── mbr_stub.S                      # MBR 存根
├── kernel/
│   └── CMakeLists.txt                  # 内核占位
├── test/
│   ├── CMakeLists.txt                  # 测试配置
│   ├── framework/
│   │   └── test_framework.h            # 测试框架
│   └── unit/
│       └── test_smoke.cpp              # 冒烟测试
├── scripts/
│   ├── check_toolchain.sh              # 工具链检查
│   └── build_image.sh                  # 镜像构建
└── docs/hands-on/
    └── 000-env-toolchain.md            # 本教程
```

---

> **本章 git tag**：`000_env_toolchain`
> **下一章**：`001_boot_real_mode`
