---
title: 000-env-toolchain-1 · 环境搭建
---

# 000-1 · 工具链安装与 CMake 构建系统

> **本篇目标**：安装 Cinux 开发所需的全部工具，编写检查脚本验证安装，
> 配置 CMake 构建系统的完整骨架（toolchain file + 顶层 CMakeLists.txt + QEMU 集成）。
>
> **完成后的效果**：`cmake -DCMAKE_BUILD_TYPE=Debug ..` 在 build 目录里 configure 成功，
> 终端打印构建配置摘要，所有编译器 flag 正确注入。
>
> **前置知识**：熟悉 Linux 命令行，有基本的 C/C++ 经验即可。

---

## 前言：为什么要先折腾环境

搭建开发环境这件事，向来是劝退人。你在网上随便搜个 OS 开发教程，
十有八九都会告诉你"先去编译一个交叉编译器"
（孩子们，如果你是跟着《操作系统还原真相》那本书走，你收货的是 gcc 4.6.2），
然后丢给你一堆链接和命令，照着敲完也不知道自己到底干了什么。
等到后面编译报错、链接找不到符号、QEMU 起来就是黑屏的时候，
根本不知道问题出在哪，只能对着终端发呆。

笔者在折腾 Cinux 的时候也踩过不少这种坑。有些教程用 NASM 汇编器，
有些用 GRUB 引导加载，还有些直接给你一个 Docker 镜像让你"感受一下"
——这对于想理解从 MBR 到内核每一步到底发生了什么的人来说，简直是一种折磨。
如果你跟我一样想搞清楚从硬盘第一扇区开始，
CPU 到底干了什么、内存里到底发生了什么，
那就必须从最基础的工具链开始，一步一步把环境搭起来，
搞清楚每一个编译 flag 的含义。

这一章做的事情看着很 boring：装几个包，写一个检查脚本，配一下 CMake。
但这些东西是后面写 bootloader 和内核的基石
——现在偷懒跳过去，后面一定会加倍还回来。

好了，废话不多说，开始上号。

---

## 我们的环境

实验环境是一台 x86_64 Linux 机器（Ubuntu 22.04 或更新版本），
工具链完全来自系统包管理器——不需要编译交叉编译器，不需要 Docker，
不需要任何特殊准备。GCC 版本要求 11+（支持 C++23 的基本特性），
CMake 笔者使用的是 4.1（实际 3.20 以上即可），
QEMU 要求 8.0+（模拟 x86_64 启动）。
编辑器随意，但如果用 VS Code 的话，
项目里已经配好了 clangd 支持（`compile_commands.json` 在 build 目录里自动生成），
装上 clangd 插件就能获得代码补全和错误提示。

这里有一个值得展开说说的选择：为什么不用交叉编译器？
OSDev Wiki 上的 Bare Bones 教程强烈建议你构建一个 `x86_64-elf-gcc` 交叉编译器，
理由是系统 GCC 可能会隐式链接宿主系统的库、
假设宿主系统的 ABI、甚至生成一些在裸机上无法运行的代码。
这些担忧在理论上是成立的，但在实际操作中，
只要你加上 `-ffreestanding -nostdlib -static` 这些 flag，
系统 GCC 生成的代码在 x86_64 裸机上就是完全可用的
——因为我们的宿主机本身就是 x86_64，指令集层面没有任何差异。

```
Cinux 构建架构总览：

    cmake/toolchain-x86_64.cmake     <- 编译 flag 注入
               |
               v
        CMakeLists.txt (顶层)         <- 全局配置 + 子目录编排
        +-- add_subdirectory(boot)    <- MBR 编译 -> mbr.bin
        +-- add_subdirectory(kernel)  <- 内核编译（后续 tag）
        +-- add_subdirectory(test)    <- Host 端单元测试
        +-- include(cmake/qemu.cmake) <- make run/run-debug target

    编译流水线（以 MBR 为例）：

    mbr.S --+--> mbr.o --+--> mbr.elf --+--> mbr.bin --+--> cinux.img
     (汇编)    (AS -Wa,--32) (ld -m elf_i386) (objcopy -O binary)  (dd)

    辅助脚本：

    scripts/check_toolchain.sh  -> 验证工具安装
    scripts/log/logging.sh      -> 带颜色的日志函数
    scripts/build_image.sh      -> dd 组装磁盘镜像
```

---

## 概念精讲：freestanding vs hosted

### 为什么写 OS 内核不能用普通的方式编译

普通的应用程序开发，编译器默认帮你做了很多事情：
链接标准 C 库（glibc），假设操作系统提供了文件系统、
内存分配、线程调度等服务，
甚至帮你插入栈保护代码和异常处理表。
这些在写应用程序的时候是天经地义的，
但写内核的时候反而全是累赘
——因为内核本身就是那个"提供一切服务"的东西，
它运行在没有任何操作系统支撑的裸机上，
连 `printf` 都没有，更别提什么 `malloc` 和 `std::vector` 了。

编译理论里把这两种环境分得很清楚：
有操作系统支撑的叫 **hosted**（托管环境），
没有的叫 **freestanding**（独立环境）。
内核开发属于后者，
必须用一系列编译 flag 告诉编译器"别自作聪明"。

对比一下两种环境下的编译 flag 差异：

```
Hosted (普通程序)           Freestanding (内核)
─────────────────────      ──────────────────────────────
默认链接 glibc              -ffreestanding -nostdlib
栈保护 canary               -fno-stack-protector
异常处理 .eh_frame          -fno-exceptions
RTTI (dynamic_cast)         -fno-rtti
Red zone 优化               -mno-red-zone
默认代码模型 small           -mcmodel=kernel
动态链接                     -static
```

每一个 flag 背后都有具体的技术原因。

`-ffreestanding` 告诉编译器代码运行在没有操作系统的裸机环境，
只提供 freestanding 头文件（`<stdint.h>`, `<stddef.h>`, `<stdarg.h>` 等），
不提供 `printf`、`malloc`。

`-fno-stack-protector` 禁用栈保护，
因为栈保护的 canary 初始化依赖 `__stack_chk_fail` 函数，
这个函数需要标准库提供，内核里什么都没有。

`-mno-red-zone` 禁用 x86_64 System V ABI 里的 red zone
——这是给用户态程序的一个 128 字节栈下空间优化，
允许函数在不调整 RSP 的情况下使用栈空间。
但在内核里，中断可能在任何时候打断执行（包括红区里的代码），
如果用了 red zone，中断处理程序的栈帧会覆盖掉还没用完的数据，
直接导致数据损坏和难以调试的随机崩溃。

`-mcmodel=kernel` 告诉编译器代码会运行在地址空间最顶端
（高半核，约 `0xFFFFFFFF80000000`），
这影响了它生成绝对地址引用的方式。

C++ 特有的 flag 禁用了异常（`-fno-exceptions`）和 RTTI（`-fno-rtti`）。
为什么 OS 内核不能用异常？
原因在于异常的展开（unwinding）需要 `.eh_frame` 段
和 `__cxa_begin_catch` 等运行时函数的支持，
这些都需要标准库提供。
而且异常展开会隐式调用析构函数，
这要求堆分配器已经初始化
——但堆分配器本身就是内核的一部分，形成了循环依赖。

对比一下其他项目的做法也很有参考价值：

| 项目 | 构建方案 | 复杂度 |
|------|----------|--------|
| xv6 | 裸 Makefile，flag 硬编码 | 低，但不可扩展 |
| SerenityOS | 30+ CMake 模块，多编译器多架构 | 高，工业级 |
| Cinux | CMake + toolchain file，最小配置 | 中等，刚好够用 |

---

## Step 1: 安装并验证工具链

**目标**：安装 Cinux 开发所需的全部工具，确认版本满足要求。

**设计思路**：Cinux 的构建依赖 GNU 工具链
（gcc, g++, as, ld, objcopy）和 QEMU 模拟器，构建系统使用 CMake。
我们需要确保这些工具都已安装且版本足够新
——特别是 CMake 需要支持现代特性
（toolchain file、generator expressions 等），
GCC 需要支持 C++23 标准。
Cinux 还需要 `gcc-multilib` 或等价包来支持 32 位交叉汇编
——bootloader 的 MBR 和 stage2 运行在 16 位和 32 位模式下，
需要用 32 位 ELF 格式链接（`-Wl,-m,elf_i386`），
即使最终的 64 位内核不需要。

**实现约束**：

需要编写两个 Shell 脚本。

第一个是日志模块 `scripts/log/logging.sh`，
它导出颜色变量（`LOG_RED`, `LOG_GREEN`, `LOG_YELLOW`, `LOG_NC`）
并定义四个日志函数：
`log_info`（绿色 `[INFO]`）、
`log_error`（红色 `[ERROR]`，输出到 stderr）、
`log_warn`（黄色 `[WARN]`）和
`log_success`（绿色 `[SUCCESS]`）。
所有函数使用 ANSI 转义序列给终端输出加颜色，
脚本之间通过 `source` 命令引入复用。

第二个是检查脚本 `scripts/check_toolchain.sh`，
它先 `source` 日志模块，
然后逐个检查以下工具：
`gcc`, `g++`, `as`（GNU 汇编器）, `ld`（链接器）,
`objcopy`（二进制转换工具）, `qemu-system-x86_64`, `cmake`。
检查使用 `command -v` 检测命令是否存在，
不存在则打印缺失提示和安装命令后 `exit 1` 退出。
CMake 版本检查是单独的：
用 `cmake --version | head -n1 | grep -oP '\d+\.\d+'` 提取版本号，
然后和最低要求做数值比较。
任何一个工具缺失都会终止脚本，
而不是继续往下检查
——因为后续工具的检测可能依赖前面的工具。

**踩坑预警**：如果你的系统没有安装 `gcc-multilib`，
在链接 32 位 bootloader 目标文件时会报 `cannot find -lgcc_s` 之类的错误。
因为 MBR 和 stage2 需要 32 位 ELF 链接，
即使目标是 64 位内核，32 位支持也不能少。
在 Ubuntu 上需要额外安装 `gcc-multilib` 和 `g++-multilib` 包。
这个报错信息完全不指向真正的原因，笔者在这里被坑了半天。

**验证**：运行检查脚本（`./scripts/check_toolchain.sh`），
预期看到每个工具后面都跟着绿色的 `[OK]` 标记，
最后一行显示 `[OK] All required tools are installed!`。
如果有任何工具缺失，脚本会打印对应的安装提示并退出。

---

## Step 2: 配置 CMake 构建系统

**目标**：搭建 CMake 项目骨架，
包括顶层 CMakeLists.txt、toolchain file 和 QEMU 集成。

**设计思路**：Cinux 的构建系统采用三层结构：
顶层 `CMakeLists.txt` 负责项目全局配置
（最低版本、语言、编译选项、子目录），
`cmake/toolchain-x86_64.cmake` 定义裸机交叉编译的具体参数，
`cmake/qemu.cmake` 注册 QEMU 相关的 make target
（`make run`, `make run-debug` 等）。
这种分层设计让每个文件的职责非常清晰：
toolchain file 管"怎么编译"，
qemu.cmake 管"怎么运行"，
顶层 CMakeLists 管"编译什么"。

**实现约束**：

**Toolchain file** (`cmake/toolchain-x86_64.cmake`)：
这个文件是 CMake 交叉编译的核心。
它需要设置 `CMAKE_SYSTEM_NAME` 为 `Generic`
（告诉 CMake 没有目标操作系统）、
`CMAKE_SYSTEM_PROCESSOR` 为 `x86_64`，
然后通过 `CMAKE_C_FLAGS_INIT`、`CMAKE_CXX_FLAGS_INIT`、
`CMAKE_ASM_FLAGS_INIT`、`CMAKE_EXE_LINKER_FLAGS_INIT`
注入编译和链接 flag。
注意这里必须用 `_INIT` 后缀而不是直接设 `CMAKE_C_FLAGS`
——`_INIT` 变量只在第一次 configure 时被读取，
之后用户可以通过命令行追加自定义 flag 而不会和 toolchain file 的 flag 冲突，
是 CMake 官方推荐的 toolchain file 写法。

链接器 flag 包括 `-nostdlib`
（不链接标准启动文件和库）和 `-static`
（纯静态链接不依赖动态链接器）。
最后通过 `CMAKE_FIND_ROOT_PATH_MODE_*` 设置查找策略：
`PROGRAM NEVER` 表示 `find_program` 只在宿主系统搜索
（我们需要在宿主上找 CMake、objcopy 这些工具），
`LIBRARY ONLY` 和 `INCLUDE ONLY`
配合空的 `CMAKE_FIND_ROOT_PATH`
禁用所有库和头文件的自动查找，
内核项目必须手动指定所有依赖。

**顶层 CMakeLists.txt**：
需要设置 CMake 最低版本
（笔者环境为 4.1，实际 3.20 以上即可），
声明项目名称（`cinux`）、版本（`0.1.0`）
和使用的语言（C、C++、ASM）。
如果没有通过命令行指定 `CMAKE_TOOLCHAIN_FILE`，
就自动使用项目里的 toolchain file 并写入 cache。
然后添加通用编译选项（`-Wall -Wextra`），
导出 `compile_commands.json`（给 clangd/IDE 用），
设置 Debug（`-g -O0`）和 Release（`-O2 -DNDEBUG`）模式的编译 flag。
最后通过 `add_subdirectory` 引入 boot 和 kernel 子目录，
通过 `option(CINUX_BUILD_TESTS)` 控制 test 子目录的引入，
并通过 `include(cmake/qemu.cmake)` 引入 QEMU 配置。

**QEMU 配置** (`cmake/qemu.cmake`)：
使用 `find_program` 查找 `qemu-system-x86_64`，
注册 `make run` 和 `make run-debug`
（带 GDB server）两个自定义 target。
QEMU 的启动参数需要包含：
`-m` 指定内存大小、
`-serial stdio` 把串口重定向到终端、
`-no-reboot` 防止 triple fault 后重启、
`-debugcon` 开启调试端口
（Bochs debug console，I/O port 0xE9）。
`make run-debug` 会额外加上 `-s`
（开启 GDB stub 在端口 1234）
和 `-S`（启动后暂停等待 GDB 连接）。
镜像构建脚本 `scripts/build_image.sh`
被 CMake 调用来把 MBR 裸二进制和其他组件写入磁盘镜像。

**踩坑预警**：CMake 的 toolchain file 在项目第一次 configure 时读取，
之后修改 toolchain file 不会自动生效
——必须清空 build 目录重新 configure。
另外，`CMAKE_SYSTEM_NAME Generic` 这个值很关键，
写错（比如写成 `Linux` 或留空）
会导致 CMake 按照 Linux 程序的方式来配置构建。
笔者有一次手滑写成了 `Linux`，
后面所有的编译都正常通过，
但链接出来的内核一跑就 triple fault
——排查了整整两个小时才发现是这一个词的问题。

**验证**：在项目根目录创建 build 目录并运行 CMake configure
（`mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug ..`）。
如果一切正常，终端会打印构建配置摘要，
包括编译器路径、构建类型、toolchain file 路径等信息。
确认 CXX Compiler 显示的是系统 GCC 路径，
Toolchain 显示的是项目里的 toolchain file。

---

## 调试技巧

### 问题：CMake configure 失败，报找不到 C++23

这说明 GCC 版本太旧。C++23 需要至少 GCC 11
（部分特性可能需要更新的版本）。
运行 `g++ --version` 检查版本号。
如果确实太旧，需要升级 GCC。

### 问题：链接 MBR 时报 "cannot find -lgcc_s" 或类似的 32 位相关错误

这是因为缺少 32 位支持库。
MBR 和后续的 stage2 需要 32 位 ELF 链接（`-m elf_i386`），
而 64 位系统默认不带 32 位库。
安装 `gcc-multilib` 和 `g++-multilib` 包即可解决。

### 问题：改了 toolchain flag 但 CMake configure 没有变化

CMake 缓存了上一次的 configure 结果。
删掉整个 build 目录（`rm -rf build`），重新 configure 即可。

### 问题：想确认 toolchain flag 是否真的被注入了

可以在 build 目录里查看 `compile_commands.json`
（如果项目有源文件的话），
或者运行 `cmake --build build -- VERBOSE=1` 查看完整的编译命令，
确认每条编译命令里都带着 `-ffreestanding -mno-red-zone` 等 flag。

---

## 本篇小结

| 概念 | 要点 |
|------|------|
| Freestanding 环境 | 内核不能用标准库，需要 `-ffreestanding -nostdlib` 等 flag |
| CMake toolchain file | `CMAKE_SYSTEM_NAME Generic` 告诉 CMake 无目标 OS |
| `_INIT` 后缀 | toolchain file 必须用 `_INIT` 变量，允许用户追加 flag |
| `-mno-red-zone` | 内核不能有 red zone，中断会覆盖未保存数据 |
| `-mcmodel=kernel` | 代码运行在地址空间最顶端（高半核） |
| multilib | MBR/stage2 需要 32 位 ELF 链接，必须安装 `gcc-multilib` |
| 构建架构 | 三层结构：toolchain file + 顶层 CMakeLists + QEMU 配置 |

到这里我们已经完成了开发环境的基础搭建：
安装了全套工具链，
理解了 freestanding 编译的核心概念和每一个 flag 的含义，
配好了 CMake 构建系统的骨架。
下一篇我们将在这个基础上编写第一个 MBR 存根，
把从汇编源码到 QEMU 启动的完整构建链条跑通。

---

## 扩展方向

- 在 toolchain file 里加 `-Werror` 把所有警告变成错误，强制保持代码质量
- 把 `CMAKE_SYSTEM_NAME` 改成 `Linux` 然后 `make`，观察链接错误的变化，理解为什么必须是 `Generic`
- 写一个 CMake Presets 文件封装 Debug/Release 配置，练习现代 CMake 的工作流
- 尝试引入 CMake Presets（`CMakePresets.json`），把常用的 configure 配置封装成预设，用户只需要 `cmake --preset debug` 一条命令

## 参考资料

- CMake 官方文档 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html): toolchain file 的所有变量和用法
- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones): freestanding 编译 flag 和交叉编译基础
- Intel SDM — Volume 1: Basic Architecture，`-mcmodel=kernel` 和 `-mno-red-zone` 的底层原因
- SerenityOS — [CMake 构建系统](https://github.com/SerenityOS/serenity): 生产级 OS 的 CMake 架构参考
