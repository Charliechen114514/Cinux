# 从零写个操作系统（一）：我被"交叉编译工具链"劝退的三十天

> 作者：
> 标签：x86-64, bootloader, GNU AS, AT&T 汇编, QEMU, 裸机开发, CMake, C++23

---

## 前言：我想写个 OS，结果第一步就被劝退了

说实话，这个念头我憋很久了。写着写着 C++ 代码，总好奇底层到底是咋跑的。那些大佬张口就来"实模式""保护模式""长模式"，听得我一愣一愣的，但网上找个能跟着敲的教程，比找对象还难。

不是太老，就是太复杂。有的教程十年前写的，代码贴一半就断更；有的上来让你装个 Docker 镜像，所有东西都封装好了，你根本不知道自己干了啥；还有的用 NASM，跟我习惯的 GCC 玩不到一块。我就想找个**中文**、**从零开始**、用**现代 C++** 的 OS 教程，结果找了一圈，决定：算了，我自己趟一遍坑，把每一步都记下来。

这一章我们做的事情不算多：装工具、写检查脚本、配置 CMake、写个只有三行指令的 MBR 存根，再跑个 `1+1=2` 的测试。但这些东西都是后面写 bootloader 和内核的基础，现在偷懒跳过去，后面一定会还回来。

好了，废话不多说，我们开始上号。

---

## 环境说明

先说下我的环境，免得你踩一样的坑：

| 项目 | 版本/说明 |
|------|-----------|
| 系统 | WSL2 (Ubuntu) |
| GCC  | 15.2（WSL 自带） |
| QEMU | `qemu-system-x86_64` |
| CMake | 3.20+ |

一开始我也纠结过要不要装个专门的交叉编译工具链，比如 `x86_64-elf-gcc` 之类的东西。但转念一想，先别折腾，系统自带的工具能不能跑？跑不通再说。

结果还真跑通了。

---

## 阶段一：工欲善其事 —— 工具链检查脚本

在开始写代码之前，我们得先确认手头的工具够不够用。gcc、g++、ld、objcopy、qemu、cmake，少一个都不行。

手敲命令检查太蠢了，写个脚本自动搞定：

```bash
#!/bin/bash
# scripts/check_toolchain.sh

check_command() {
    if command -v "$1" &> /dev/null; then
        echo -e "${GREEN}[OK]${NC} $1 found: $(command -v $1)"
        return 0
    else
        echo -e "${RED}[FAIL]${NC} $1 not found"
        return 1
    fi
}

# 检查 GCC
if gcc --version &> /dev/null; then
    echo -e "${GREEN}[OK]${NC} gcc: $(gcc --version | head -n1)"
else
    echo -e "${RED}[FAIL]${NC} gcc not found"
fi
```

这里用了 `command -v` 而不是传统的 `which`，原因很简单：`command -v` 是 POSIX 标准命令，在各种 shell 里行为一致，而 `which` 在某些环境下可能不靠谱。

加个颜色输出让结果一目了然 —— [OK] 是绿色，[FAIL] 是红色。肉眼快速扫描比读一坨纯文本舒服多了。

脚本还会检查 CMake 版本，因为 Cinux 用到了一些现代 CMake 特性。版本检查通过解析 `cmake --version` 的输出，提取版本号，然后用 `bc` 进行数值比较。这一步很容易踩坑，因为不同系统的输出格式可能不太一样。

写完记得加执行权限：

```bash
chmod +x scripts/check_toolchain.sh
./scripts/check_toolchain.sh
```

看到一排绿色的 [OK]，说明工具链准备就绪。

---

## 阶段二：核心 —— 用系统 GCC 编译裸机代码

这里是整个环境搭建的核心，也是最容易让人劝退的地方。

很多教程会告诉你："去装个交叉编译工具链吧，`x86_64-elf-gcc` 之类的。"但你有没有想过，系统的 GCC 到底能不能编译内核代码？答案是：能，关键是告诉它"别瞎自作聪明"。

### 什么是 freestanding 环境？

OS 开发和普通应用开发最大的区别在于：内核运行在没有操作系统的裸机上，不能依赖任何标准库。这种环境叫"freestanding 环境"。

好消息是，你**不需要**安装专门的交叉编译工具链。系统的 GCC 完全可以编译内核，只要把编译选项打对就行。

### GCC 编译选项逐条拆解

创建一个 `cmake/toolchain-x86_64-elf.cmake` 文件，虽然名字带 `elf`，但实际用的还是系统 gcc：

```cmake
# 告诉 CMake 这是 freestanding 环境
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 直接用系统的 gcc/g++
find_program(CMAKE_C_COMPILER NAMES gcc gcc-11 gcc-12 gcc-13 gcc-14 gcc-15)
find_program(CMAKE_CXX_COMPILER NAMES g++ g++-11 g++-12 g++-13 g++-14 g++-15)

# 编译标志：这是核心！
set(CMAKE_C_FLAGS_INIT "
    -ffreestanding           # 别假设有标准库
    -fno-stack-protector     # 禁用栈保护（内核自己管栈）
    -mno-red-zone            # 禁用红区优化（中断处理需要！）
    -mcmodel=kernel          # 内核代码模型
    -Wall -Wextra            # 启用警告
")

# C++ 额外加这些
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}
    -fno-exceptions          # 禁用异常（内核自己实现）
    -fno-rtti                # 禁用运行时类型信息
    -std=c++23               # 用现代 C++
")

# 链接器标志
set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib                # 不链接标准库
    -static                  # 静态链接
")
```

来逐条解释一下这些选项到底干了啥：

- **`CMAKE_SYSTEM_NAME=Generic`**：这是 CMake 的"freestanding 模式"开关。设置成 `Generic` 后，CMake 就不会去寻找标准库和运行时组件。

- **`-ffreestanding`**：告诉编译器"别假设有标准库"。这会影响编译器的很多优化决策，比如它不会假设某些函数一定存在。

- **`-fno-stack-protector`**：禁用栈保护。普通的程序会有栈金丝雀（stack canary）来检测栈溢出，但内核需要自己管理栈，这个保护机制反而会坏事。

- **`-mno-red-zone`**：**这个坑了我半天**。x86-64 有个"红区"优化，编译器会在栈指针下方留 128 字节的空间，用来存放临时变量。但问题来了，中断发生的时候，CPU 会直接往栈上压数据，可能就把红区给覆盖了。你不加这个选项，就会收获一个非常漂亮的 kernel panic。

- **`-mcmodel=kernel`**：使用内核代码模型。普通的代码模型假设代码链接在低地址（低于 2GB），但内核可能链接到更高的地址。

- **`-nostdlib`**：告诉链接器不要链接标准库。libgcc、libc、libstdc++ 统统不要。

到这里你可能有个疑问：不用标准库，那 `memcpy`、`memset` 这些函数怎么办？答案是：自己写。或者链接 libgcc 的某些部分。但那是后面的事了，现在先把环境搭起来。

---

## 阶段三：最简 MBR —— 三行代码启动

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

最后两个字节必须是 `0xAA55`，否则 BIOS 会认为这不是一个可启动的磁盘。

我们用 AT&T 汇编语法写一个最简 MBR。AT&T 语法和 Intel 语法的区别在于：操作数顺序相反（源在前、目标在后），寄存器加 `%`，立即数加 `$`。

```asm
# boot/mbr_stub.S
.section .text
.code16                      # 16 位实模式代码
.global _start

_start:
    cli                      # 关中断
    hlt                      # 停机
    jmp _start               # 无限循环

# 填充到 510 字节，然后写签名
.fill 510 - (. - _start), 1, 0
.word 0xAA55                 # 签名（小端序存储为 55 AA）
```

代码只有三条指令：`cli` 清除 IF 位禁用可屏蔽中断，`hlt` 停止 CPU 执行，`jmp` 跳回自己形成无限循环。这只是一个存根，用来验证构建链是否正常。后面几章我们会在此基础上扩展成完整的 bootloader。

这里有个容易搞反的地方：`.word 0xAA55` 在 GAS（GNU Assembler）里是小端序写入的，所以磁盘上实际是 `55 AA`。BIOS 读取后会当成 `AA55`，这是对的。别搞反了，否则 BIOS 会拒绝启动。

---

## 阶段四：自研测试框架 —— 为什么不用 Google Test

测试是 OS 开发的重要组成部分，但我们不想引入第三方依赖。Google Test 虽然强大，但也带来了外部依赖。我们的目标是自给自足。

核心思路很简单：用一个全局数组存储所有测试，每个测试用 `TEST()` 宏自动注册。

```cpp
// test/framework/test_framework.h

#define TEST(test_name)                                                 \
    static void _test_fn_##__LINE__();                                  \
    static struct _TestAutoReg_##__LINE__ {                             \
        _TestAutoReg_##__LINE__() {                                     \
            _register_test(test_name, __FILE__, __LINE__,               \
                           _test_fn_##__LINE__);                        \
        }                                                               \
    } _test_reg_##__LINE__;                                             \
    static void _test_fn_##__LINE__()

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
```

`TEST()` 宏的魔法在于：每个测试块会生成一个测试函数和一个全局对象，对象的构造函数会自动调用 `_register_test()` 把测试注册到全局数组里。这样我们就不用手动维护测试列表了。

框架还支持两种运行模式：Host 模式用 `printf` 输出，在 Linux 上跑；QEMU 模式用 `serial_printf` 输出（后面实现串口驱动后再用）。

先写个冒烟测试验证框架本身不炸：

```cpp
// test/unit/test_smoke.cpp

TEST("smoke: 1+1=2") {
    int result = 1 + 1;
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(result == 2);
}

TEST("smoke: pointer assertions") {
    int* null_ptr = nullptr;
    ASSERT_NULL(null_ptr);

    int value = 42;
    int* valid_ptr = &value;
    ASSERT_NOT_NULL(valid_ptr);
    ASSERT_EQ(*valid_ptr, 42);
}
```

跑一下看看：

```bash
cd build
make test_smoke
./test/unit/test_smoke
```

应该看到绿色的 `[PASS]` 消息。到这里测试框架就 OK 了，后面可以添加更多测试。

---

## 阶段五：CMake + QEMU 一键启动

现在我们已经有了 MBR 代码和测试框架，接下来要把它们串起来，用一条命令就能跑起来。

顶层 `CMakeLists.txt` 是整个构建系统的入口：

```cmake
cmake_minimum_required(VERSION 3.20)
project(cinux VERSION 0.1.0 LANGUAGES C CXX ASM)

# 如果没指定工具链文件，用项目自带的
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64-elf.cmake"
        CACHE FILEPATH "Toolchain file")
endif()

add_subdirectory(boot)
add_subdirectory(kernel)

option(CINUX_BUILD_TESTS "Build host-side tests" ON)
if(CINUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

include(cmake/qemu.cmake)
```

这里有个小技巧：如果用户没有显式指定 `CMAKE_TOOLCHAIN_FILE`，我们会自动使用项目自带的工具链文件。这样新手上手更容易，不用记住长长的 cmake 命令参数。

`cmake/qemu.cmake` 定义了几个自定义目标，让开发者可以用简单的 `make run` 命令启动 QEMU：

```cmake
find_program(QEMU_EXECUTABLE qemu-system-x86_64)

set(QEMU_COMMON_FLAGS "
    -m 256M                 # 分配 256MB 内存
    -serial stdio           # 串口重定向到标准输入输出
    -no-reboot              # panic 时不自动重启
    -no-shutdown            # 关机后不退出 QEMU
")

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU (serial: stdio)"
)
```

`-serial stdio` 把串口重定向到标准输入输出，这样内核的串口日志会直接显示在终端里。`-no-reboot` 和 `-no-shutdown` 让 QEMU 在 panic 时不自动重启或退出，方便查看错误信息。

镜像生成脚本 `scripts/build_image.sh` 用 `dd` 命令把 MBR 写入磁盘镜像：

```bash
# 创建空白镜像（1MB）
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# 写入 MBR
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
```

这里有个坑：`conv=notrunc` 一定要加，否则 dd 会把输出文件截断成 512 字节，你的 1MB 镜像就变成 512 字节了。

脚本最后会验证 MBR 签名是否正确：

```bash
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    echo "MBR signature valid: 0xAA55"
fi
```

---

## 收尾：到这里就大功告成了

好了，所有东西都准备就绪。让我们跑一遍：

```bash
# 创建构建目录
mkdir build && cd build

# 配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 编译
make

# 运行测试
make test

# 启动 QEMU
make run
```

你应该能看到类似这样的输出：

```
=== Cinux Build Configuration ===
  Project:     cinux v0.1.0
  Build type:  Debug
  C Compiler:  /usr/bin/gcc
  CXX Compiler:/usr/bin/g++
  ASM Compiler:/usr/bin/as
===================================

=== Cinux Test Runner ===
Running 3 test(s)...

[PASS] smoke: 1+1=2
[PASS] smoke: boundary values
[PASS] smoke: pointer assertions

=== Results: 3 passed, 0 failed ===
[SUITE PASSED]

Building disk image: build/cinux.img
MBR signature valid: 0xAA55

Starting QEMU (serial: stdio)
```

QEMU 会启动一个窗口。由于我们的 MBR 只是 `cli; hlt; jmp`，所以屏幕是黑的 —— 这很正常。按 `Ctrl+A` 然后按 `X` 退出 QEMU。

到这里，我们已经完成了：

1. ✅ 工具链检查脚本
2. ✅ 用系统 GCC 配置 freestanding 编译环境
3. ✅ 最简 MBR 存根（三行代码）
4. ✅ 自研测试框架
5. ✅ CMake + QEMU 一键启动

### 意外收获

折腾完这一套，我发现一个有趣的结论：其实**真的不需要专门的交叉编译工具链**。系统的 gcc/g++ 只要打上正确的选项，完全可以胜任内核编译的任务。这比去折腾什么 `x86_64-elf-gcc` 简单多了。

### 下一步预告

下一章我们会：
- 在 MBR 中添加屏幕输出（通过 BIOS INT 10h）
- 实现磁盘读取（通过 BIOS INT 13h 扩展读取）
- 编写 Stage2 bootloader
- 切换到保护模式

完成下一章后，我们会有一个可以在屏幕上显示文字的 bootloader，为后续进入长模式打下基础。

**本章 git tag**：`000_env_toolchain`
