# Phase 0 · 环境与工具链

> **本章目标**：从零搭建 Cinux 开发环境，验证工具链可用，构建并运行第一个 MBR 存根，跑通自研测试框架。
>
> **完成后的效果**：`make run` 启动 QEMU，屏幕左上角出现蓝底白字的 "C"；`make test` 全绿通过冒烟测试。
>
> **前置知识**：熟悉 Linux 命令行，有基本的 C/C++ 经验即可。

---

## 前言：为什么要先折腾环境

搭建开发环境这件事，向来是劝退人。你在网上随便搜个 OS 开发教程，十有八九都会告诉你"先去编译一个交叉编译器（孩子们，如果你是跟着《操作系统还原真相》那本书走，你收货的是gcc 4.6.2）"，然后丢给你一堆链接和命令，照着敲完也不知道自己到底干了什么。等到后面编译报错、链接找不到符号、QEMU 起来就是黑屏的时候，根本不知道问题出在哪，只能对着终端发呆。

笔者在折腾 Cinux 的时候也踩过不少这种坑。有些教程用 NASM 汇编器，有些用 GRUB 引导加载，还有些直接给你一个 Docker 镜像让你"感受一下"——这对于想理解从 MBR 到内核每一步到底发生了什么的人来说，简直是一种折磨。如果你跟我一样想搞清楚从硬盘第一扇区开始，CPU 到底干了什么、内存里到底发生了什么，那就必须从最基础的工具链开始，一步一步把环境搭起来，搞清楚每一个编译 flag 的含义。

这一章做的事情看着很 boring：装几个包，写一个检查脚本，配 CMake，写一个只有几条指令的 MBR 存根，再跑一个 `1+1=2` 的测试。但这些东西是后面写 bootloader 和内核的基石——现在偷懒跳过去，后面一定会加倍还回来。

好了，废话不多说，开始上号。

---

## 概念精讲

### 为什么写 OS 内核不能用普通的方式编译

普通的应用程序开发，编译器默认帮你做了很多事情：链接标准 C 库（glibc），假设操作系统提供了文件系统、内存分配、线程调度等服务，甚至帮你插入栈保护代码和异常处理表。这些在写应用程序的时候是天经地义的，但写内核的时候反而全是累赘——因为内核本身就是那个"提供一切服务"的东西，它运行在没有任何操作系统支撑的裸机上，连 `printf` 都没有，更别提什么 `malloc` 和 `std::vector` 了。

编译理论里把这两种环境分得很清楚：有操作系统支撑的叫 **hosted**（托管环境），没有的叫 **freestanding**（独立环境）。内核开发属于后者，必须用一系列编译 flag 告诉编译器"别自作聪明"：不要链接标准库，不要插入异常处理代码，不要加栈保护，不要假设有红区（red zone，x86_64 ABI 里给用户态程序留的一个 128 字节优化区域，内核里绝对不能有），还要告诉它代码会跑在最高的特权级（`-mcmodel=kernel`）。

很多 OS 教程推荐你先编译一个完整的交叉编译器（比如 `x86_64-elf-gcc`），这个做法确实更"正统"，但代价是你得花很长时间等 GCC 编译完，而且出了问题排查起来更痛苦。Cinux 选择的方案更加务实：直接用系统自带的 GCC，通过 CMake toolchain file 注入正确的编译 flag。因为我们的宿主机器就是 x86_64 Linux，目标架构也是 x86_64，所以系统 GCC 生成的代码在指令集层面是完全兼容的，只需要关掉那些"帮倒忙"的默认行为就行。

### MBR 是什么，为什么它是 OS 的起点

当你按下电脑电源键的那一刻，CPU 从一个固定的物理地址开始执行固件代码（BIOS 或 UEFI），这段代码做完硬件自检（POST）之后，会从启动盘的第一个扇区读取 512 字节数据，加载到物理内存地址 `0x7C00`，然后把控制权交给它。这个 512 字节就是 **MBR（Master Boot Record）**，它是整个操作系统启动链条的第一环。

为什么是 512 字节？因为硬盘的一个扇区就是 512 字节，这是 IBM PC 时代留下来的约定。为什么是 `0x7C00`？这同样是 BIOS 规范定义的固定地址，所有的 x86 BIOS 都会在这个地址加载 MBR，不会有例外。MBR 的最后两个字节必须是 `0x55` 和 `0xAA`（合在一起看是 `0xAA55`），BIOS 会检查这个"魔数"，如果不对就认为这不是一个有效的启动扇区，直接报错。

MBR 虽然只有 512 字节（而且扣掉分区表和签名之后实际可用的只有 446 字节左右），但它可以做很多事情——加载更多的扇区到内存、初始化显示模式、进入保护模式甚至长模式，然后把内核加载进来并跳转过去。当然，这是后面章节的事情。在 tag 000 里，我们的 MBR 只是一个"存根"：清屏、打印一个字母、停机。它的唯一目的是验证整个构建链条是通的——从汇编源码到目标文件、从链接到 objcopy 生成裸二进制、从 dd 写入磁盘镜像到 QEMU 启动。

### VGA 文本模式：最简单的屏幕输出方式

在还没有显卡驱动、没有字体渲染器的时候，怎么在屏幕上显示字符？答案是 VGA 文本模式——这是一种硬件级的文本显示机制，由 VGA 兼容显卡直接支持，不需要安装任何驱动。

文本模式的显存被映射到物理地址 `0xB8000`，它不是一块普通的内存，而是显卡直接监控的区域——你往这里写数据，屏幕上立刻就能看到对应的变化。屏幕被分成 80 列 × 25 行共 2000 个字符单元，每个单元占 2 字节：第一个字节是字符的 ASCII 码，第二个字节是显示属性。属性字节的低 4 位控制前景色，中间 3 位控制背景色，最高位控制是否闪烁。比如属性值 `0x1F` 就是蓝底白字（背景色 1=蓝色，前景色 F=亮白色）。

这意味着在实模式下（BIOS 刚把 MBR 加载进来的时候），我们只需要把段寄存器 ES 指向 `0xB800` 段（实模式下段基址需要左移 4 位，`0xB800 × 16 = 0xB8000`），然后用普通的内存写入指令往 ES:DI 写数据，就能在屏幕上看到字符了。不需要调任何 BIOS 中断，不需要初始化任何硬件——直接写内存就行。

### AT&T 汇编语法：和 Intel 语法反着来

Cinux 使用 GNU AS 汇编器，它默认采用 AT&T 语法。如果你之前看过的教程用的是 NASM（Intel 语法），刚接触 AT&T 语法会有点别扭，因为操作数顺序是反的：AT&T 语法里是"源操作数, 目标操作数"，Intel 语法里是"目标操作数, 源操作数"。另外 AT&T 语法里寄存器前面要加 `%`，立即数前面要加 `$`，内存寻址用 `offset(base, index, scale)` 的格式。习惯了就好，后面全篇都会用这个语法。

---

## 动手实现

### Step 1: 安装并验证工具链

**目标**：安装 Cinux 开发所需的全部工具，确认版本满足要求。

**设计思路**：Cinux 的构建依赖 GNU 工具链（gcc, g++, as, ld, objcopy）和 QEMU 模拟器，构建系统使用 CMake。我们需要确保这些工具都已安装且版本足够新——特别是 CMake 需要支持现代特性（toolchain file、generator expressions 等），GCC 需要支持 C++23 标准。Cinux 还需要 `gcc-multilib` 或等价包来支持 32 位交叉汇编（bootloader 的 MBR 和 stage2 运行在 16 位和 32 位模式下，需要用 32 位 ELF 格式链接）。

在 Ubuntu/Debian 上，安装命令覆盖 gcc、g++、binutils（包含 as, ld, objcopy）、QEMU 和 CMake。安装完成后，我们提供一个检查脚本来验证每个工具是否存在且版本达标。

**实现约束**：

需要编写一个 Shell 脚本 `scripts/check_toolchain.sh`，它会逐个检查以下工具：`gcc`, `g++`, `as`（GNU 汇编器）, `ld`（链接器）, `objcopy`（二进制转换工具）, `qemu-system-x86_64`, `cmake`。对于 cmake 还需要检查版本号是否满足最低要求。脚本使用 `command -v` 检测命令是否存在，不存在则打印缺失提示和安装命令后退出。检查脚本自身依赖一个日志模块 `scripts/log/logging.sh`，它提供带颜色标签的 `log_info`、`log_error`、`log_success` 函数。

**踩坑预警**：如果你的系统没有安装 `gcc-multilib`，在链接 32 位 bootloader 目标文件时会报 `cannot find -lgcc_s` 之类的错误。因为 MBR 和 stage2 需要 32 位 ELF 链接，即使目标是 64 位内核，32 位支持也不能少。在 Ubuntu 上需要额外安装 `gcc-multilib` 和 `g++-multilib` 包。

**验证**：运行检查脚本，预期看到每个工具后面都跟着 `[OK]` 标记，最后一行显示 `[OK] All required tools are installed!`。如果有任何工具缺失，脚本会打印对应的安装提示。

### Step 2: 配置 CMake 构建系统

**目标**：搭建 CMake 项目骨架，包括顶层 CMakeLists.txt、toolchain file 和 QEMU 集成。

**设计思路**：Cinux 的构建系统采用三层结构：顶层 `CMakeLists.txt` 负责项目全局配置（最低版本、语言、编译选项、子目录），`cmake/toolchain-x86_64.cmake` 定义裸机交叉编译的具体参数，`cmake/qemu.cmake` 注册 QEMU 相关的 make target（`make run`, `make run-debug` 等）。这种分层设计让每个文件的职责非常清晰：toolchain file 管"怎么编译"，qemu.cmake 管"怎么运行"，顶层 CMakeLists 管"编译什么"。

**实现约束**：

**Toolchain file** (`cmake/toolchain-x86_64.cmake`)：这个文件是 CMake 交叉编译的核心。它需要设置 `CMAKE_SYSTEM_NAME` 为 `Generic`（告诉 CMake 没有目标操作系统）、`CMAKE_SYSTEM_PROCESSOR` 为 `x86_64`，然后通过 `CMAKE_C_FLAGS_INIT`、`CMAKE_CXX_FLAGS_INIT`、`CMAKE_ASM_FLAGS_INIT`、`CMAKE_EXE_LINKER_FLAGS_INIT` 注入编译和链接 flag。C 编译器需要的 flag 包括：`-ffreestanding`（无 OS 环境）、`-fno-stack-protector`（禁用栈保护）、`-mno-red-zone`（禁用 red zone）、`-mcmodel=kernel`（内核代码模型）。C++ 编译器在此基础上还需要 `-fno-exceptions`（禁用异常）、`-fno-rtti`（禁用 RTTI）和 `-std=c++23`（使用 C++23 标准）。链接器需要 `-nostdlib` 和 `-static`。最后通过 `CMAKE_FIND_ROOT_PATH_MODE_*` 设置查找策略：程序在宿主系统找，库和头文件只在目标环境找。

**顶层 CMakeLists.txt**：需要设置 CMake 最低版本（笔者环境为 4.1，实际 3.20 以上即可），声明项目名称、版本和使用的语言（C、C++、ASM）。如果没有通过命令行指定 `CMAKE_TOOLCHAIN_FILE`，就自动使用项目里的 toolchain file。然后添加通用编译选项（`-Wall -Wextra`），导出 compile_commands.json（给 clangd/IDE 用），设置 Debug 和 Release 模式的编译 flag。最后通过 `add_subdirectory` 引入 boot、kernel、test 子目录，并通过 `include` 引入 QEMU 配置。

**QEMU 配置** (`cmake/qemu.cmake`)：使用 `find_program` 查找 `qemu-system-x86_64`，注册 `make run` 和 `make run-debug`（带 GDB server）两个自定义 target。QEMU 的启动参数需要包含：`-m` 指定内存大小、`-serial stdio` 把串口重定向到终端、`-no-reboot` 防止 triple fault 后重启、`-debugcon` 开启调试端口。

**踩坑预警**：CMake 的 toolchain file 在项目第一次 configure 时读取，之后修改 toolchain file 不会自动生效——必须清空 build 目录重新 configure。如果你改了 toolchain flag 但编译参数没变，八成是这个原因。另外，`CMAKE_SYSTEM_NAME Generic` 这个值很关键，写错（比如写成 `Linux` 或留空）会导致 CMake 按照 Linux 程序的方式来配置构建，各种 find_package 和 find_library 都会出错。

**验证**：在项目根目录创建 build 目录并运行 CMake configure。如果一切正常，终端会打印构建配置摘要，包括编译器路径、构建类型、toolchain file 路径等信息。确认 CXX Compiler 显示的是系统 GCC 路径，Toolchain 显示的是项目里的 toolchain file。

### Step 3: 编写 MBR 存根

**目标**：编写一个最小的 MBR 汇编文件，能在 QEMU 里清屏并显示一个蓝色背景的字母 "C"，然后停机。

**设计思路**：MBR 存根是整个构建链的"hello world"——它的代码逻辑极其简单（清屏、写字符、停机），但它验证了从汇编源码到最终 QEMU 运行的完整流水线：GNU AS 把 `.S` 文件汇编成 ELF 目标文件 → ld 链接成 ELF 可执行文件 → objcopy 把 ELF 转成裸二进制 → dd 写入磁盘镜像 → QEMU 从镜像启动并执行。

代码运行在 16 位实模式下（用 `.code16` 指令告诉汇编器），BIOS 将 MBR 加载到 `0x7C00` 后从这里开始执行。我们需要做的事情分三步：先把 VGA 文本模式的显存全部清成空格（2000 个字符单元），属性设为 `0x07`（黑底灰字）；然后在左上角（位置 0）写一个字符 'C'，属性设为 `0x1F`（蓝底白字）；最后关中断（`cli`）并停机（`hlt`），进入死循环防止 CPU 继续往下执行未初始化的内存。

文件末尾需要用 `.fill` 指令把剩余空间填 0，确保总长度正好 510 字节（加上最后的 2 字节签名刚好 512 字节）。签名用 `.word 0xAA55` 写入——注意这里是小端序，在文件里实际存储的是字节 `0x55 0xAA`，但 BIOS 读取时当作 `0xAA55` 来验证。

**实现约束**：

文件路径是 `boot/mbr_stub.S`，需要声明 `.section .text`、`.code16` 和 `.global _start`。入口标签是 `_start`。清屏操作使用 `rep stosw` 指令（重复存储 AX 到 ES:DI 指向的内存，每次 DI 自增 2），需要在执行前设置 ES 段寄存器为 `0xB800`、DI 为 0、AX 为 `0x0720`（空格字符 + 灰色属性）、CX 为 2000。打印字符操作直接把 AX（属性字节在 AH，字符在 AL）写入 ES:0 即可。最后需要 `.fill` 和 `.word 0xAA55` 确保文件恰好 512 字节。

同时需要编写 `boot/CMakeLists.txt` 来编译这个文件：定义一个可执行目标（名字叫 `mbr`），源文件是 `mbr_stub.S`，编译选项用 `--32`（生成 32 位目标文件以兼容 16 位代码），链接选项指定链接脚本和 `-nostdlib`。链接脚本在 CMake configure 时生成（用 `file(WRITE ...)`），设置入口地址为 `0x7C00`。最后用 `add_custom_command` 在链接后自动执行 `objcopy -O binary` 把 ELF 转成裸二进制（`mbr.bin`）。

**踩坑预警**：`.fill 510 - (. - _start), 1, 0` 这个填充指令的计算方式是"当前位置减去 `_start` 标签的位置"，如果代码部分超过 510 字节，填充值会变成负数，汇编器会报错。MBR 的代码空间非常紧张（最多 510 字节减去代码长度），但 tag 000 的存根代码很短，不会有这个问题。后续章节扩展 MBR 功能时需要特别注意。

另外，AT&T 语法里往绝对内存地址写数据的方式是 `mov %ax, %es:(0)` 或者 `movw %ax, %es:0`——注意冒号前面是段寄存器，后面是偏移量。如果你习惯 Intel 语法的 `mov word [es:0], ax`，转换过来可能会搞混操作数顺序。

**验证**：编译 MBR 目标后，确认 `build/boot/mbr.bin` 文件存在且大小恰好为 512 字节（用 `ls -la` 或 `stat` 命令查看）。用 `xxd` 查看文件的最后两个字节，应该看到 `55aa`。用 `xxd` 查看文件开头几个字节，应该能看到对应的机器码（`B8 00 B8` 之类的，具体取决于你的指令编码）。

### Step 4: 构建磁盘镜像并在 QEMU 中运行

**目标**：编写镜像构建脚本，把 MBR 裸二进制写入磁盘镜像的第一个扇区，然后用 QEMU 启动验证。

**设计思路**：QEMU 需要一个磁盘镜像文件来模拟硬盘启动。磁盘镜像本质上就是一个 raw 格式的二进制文件，字节内容和真实硬盘上的数据完全一致。MBR 必须出现在第 0 个扇区（文件偏移 0-511 字节），后续的 stage2 和内核会从第 1 个扇区开始存放。我们用 Linux 的 `dd` 命令来完成这个操作——它可以把一个文件的内容精确地写入另一个文件的指定偏移位置。

构建脚本 `scripts/build_image.sh` 接收编译产物路径作为参数，执行以下流程：先创建一个全零的空白镜像文件（1MB 足够），然后把 MBR 裸二进制写入第 0 个扇区（`dd if=mbr.bin of=image.img bs=512 count=1 conv=notrunc`），最后用 `dd` 读取镜像偏移 510-511 处的两个字节验证签名是否为 `0x55AA`。

**实现约束**：

脚本需要使用 `set -e`（任何命令失败立即退出），接收参数包括 MBR 路径和输出镜像路径（都可以有默认值）。脚本依赖 Step 1 里编写的日志模块来打印带颜色的状态信息。签名验证部分使用 `dd bs=1 skip=510 count=2` 提取最后两个字节，管道到 `xxd -p` 转成十六进制字符串，然后和 `55aa` 比较。

在 CMake 层面，`cmake/qemu.cmake` 需要注册 `make run` target，它会先依赖镜像构建（`DEPENDS image`），然后启动 QEMU 并传入磁盘镜像路径。QEMU 参数至少需要 `--drive file=cinux.img,format=raw`（指定磁盘镜像）、`-serial stdio`（串口重定向到终端，后面章节调试用）、`-no-reboot`（防止 triple fault 无限重启）。

**踩坑预警**：`dd` 命令的 `conv=notrunc` 参数非常关键——没有它的话，`dd` 会在写入完成后把输出文件截断到写入的长度，导致后续写入 stage2 和内核时文件被截断。这会在后续章节引入 stage2 后造成非常诡异的 bug（stage2 被写入但文件长度被截回 512 字节，QEMU 读到一堆零直接崩溃）。

另一个常见的坑是 QEMU 启动后屏幕一片漆黑但没有任何报错——这种情况多半是 MBR 签名写错了或者文件大小不对。用 `xxd` 检查镜像第 510-511 字节，确认是 `55aa`。

**验证**：运行 `make run`（或者直接用 QEMU 命令行指定磁盘镜像），QEMU 窗口应该弹出一个黑底屏幕，左上角显示蓝底白字的字母 "C"，然后 CPU 停机（不再有任何活动）。关闭 QEMU 窗口回到终端，没有报错。

### Step 5: 搭建测试框架并跑通冒烟测试

**目标**：实现自研的轻量测试框架，编写一个冒烟测试验证框架工作正常。

**设计思路**：内核开发没法用 Google Test 之类的标准测试框架（因为它们依赖标准库和操作系统），所以我们需要自己写一个最小的测试框架。这个框架的核心设计是：用宏来定义测试用例（`TEST("name") { ... }`），用宏来写断言（`ASSERT_EQ`, `ASSERT_TRUE` 等），然后在 main 函数里调用 `RUN_ALL_TESTS()` 自动运行所有注册的测试。

框架支持两种运行模式：Host 模式和 QEMU 模式。Host 模式用 `-DCINUX_HOST_TEST` 宏启用，测试代码用系统 g++ 编译在 Linux 上运行，`printf` 输出结果，`abort()` 处理致命错误。QEMU 模式下测试代码被编译进内核，通过串口输出结果。两种模式共享同一套 `TEST` 和 `ASSERT` 宏接口，通过条件编译切换底层实现。

冒烟测试（`test/unit/test_smoke.cpp`）不测试任何内核逻辑，只验证框架本身：测试 `TEST` 宏能否正确注册测试用例，`ASSERT_EQ` 能否正确判断相等，`ASSERT_TRUE` 和 `ASSERT_FALSE` 能否正确判断布尔值，以及边界值（零、负数、空指针、非空指针）的断言是否正常工作。

**实现约束**：

框架头文件路径是 `test/framework/test_framework.h`。它需要定义以下核心组件：

- 一个函数指针类型表示测试函数
- 一个结构体表示测试条目（包含名字、文件名、行号、函数指针）
- Host 模式下使用静态数组（最多 256 个条目）手动注册，通过全局构造函数（C++ 的静态对象初始化）在 main 函数之前自动完成注册
- 统计变量记录通过和失败的测试数量
- `TEST` 宏展开为一个静态函数定义加一个自动注册类
- `ASSERT_*` 宏使用 `do { ... } while(0)` 包裹，失败时打印文件名和行号，递增失败计数，并 return 跳出当前测试
- `RUN_ALL_TESTS` 函数遍历注册数组，依次调用每个测试函数

`test/CMakeLists.txt` 配置 Host 模式的测试：创建可执行目标 `test_smoke`，源文件只有 `test_smoke.cpp`，添加 `CINUX_HOST_TEST` 编译定义，设置包含目录（框架头文件和内核头文件），注册到 CTest。需要在顶层 CMakeLists.txt 中用 `CINUX_BUILD_TESTS` option 控制 test 子目录的引入。

**踩坑预警**：`TEST` 宏的展开有个经典的 C 预处理器陷阱——`__LINE__` 不能直接和标识符拼接（`##` 运算符不展开它的操作数），需要用两层宏间接展开。具体做法是定义一个 `_TEST_CAT(a, b)` 宏，里面再套一个 `_TEST_CAT2(a, b)` 宏做实际的 `##` 拼接。这个问题不处理好，所有测试用例会生成同一个函数名，编译直接报重复定义。

另外，`RUN_ALL_TESTS` 里的 per-test 通过/失败追踪需要仔细设计——每个测试函数失败时 return 而不是 continue，所以在循环里判断是否失败的方法是"失败计数是否有变化"而不是"当前测试是否返回了某个状态"。当前框架简化了这个问题：每次循环末尾直接打印 PASS，如果测试中途断言失败就已经 return 了不会到达这一步。

**验证**：运行 `make test`（或者 `cd build && ctest --output-on-failure`），终端应该打印测试运行器标题、测试数量、每个测试的 `[PASS]` 标记，最后显示 `Results: N passed, 0 failed` 和 `[SUITE PASSED]`。

---

## 构建与运行

完整的从零开始的构建流程如下：

首先确保工具链已安装（Step 1 的检查脚本通过），然后在项目根目录执行：创建 build 目录，进入 build 目录，运行 CMake configure（指定 Debug 模式），最后 `make` 编译。编译成功后，`make run` 启动 QEMU，`make test` 跑测试。

如果需要调试，可以用 `make run-debug` 启动 QEMU（会在端口 1234 开 GDB server，启动后暂停等待连接），然后在另一个终端用 GDB 连接：加载内核 ELF 符号文件，连接 `target remote :1234`，设置断点，`continue` 继续。Tag 000 的 MBR 存根没有符号信息（纯汇编裸二进制），GDB 调试在后续章节才有实际用途，但可以先验证 GDB 能连上。

QEMU 的常用启动参数说明：`-m` 指定模拟内存大小，`-serial stdio` 把虚拟串口映射到宿主终端（后续章节的 `kprintf` 输出会出现在这里），`-no-reboot` 让 QEMU 在 triple fault 时退出而不是重启，`-no-shutdown` 让 QEMU 在执行 `hlt` 后保持运行而不自动退出，`-s` 开启 GDB stub，`-S` 启动后暂停。

---

## 调试技巧

### 问题：QEMU 启动后黑屏，没有任何输出

排查步骤：先用 `xxd build/boot/mbr.bin | tail -1` 检查 MBR 文件最后两个字节是不是 `55aa`。如果不是，说明签名写错了或者文件大小不对（不是 512 字节）。如果签名正确但仍然黑屏，用 `xxd build/cinux.img | head -20` 检查镜像的前几十个字节，确认 MBR 内容确实被写入了第 0 扇区。如果镜像全是零，说明 `build_image.sh` 的路径参数不对。

### 问题：CMake configure 失败，报找不到 C++23

这说明 GCC 版本太旧。C++23 需要至少 GCC 11（部分特性可能需要更新的版本）。运行 `g++ --version` 检查版本号。如果确实太旧，需要升级 GCC。

### 问题：链接 MBR 时报 "cannot find -lgcc_s" 或类似的 32 位相关错误

这是因为缺少 32 位支持库。MBR 和后续的 stage2 需要 32 位 ELF 链接（`-m elf_i386`），而 64 位系统默认不带 32 位库。安装 `gcc-multilib` 和 `g++-multilib` 包即可解决。

---

## 本章小结

| 概念 | 要点 |
|------|------|
| Freestanding 环境 | 内核不能用标准库，需要 `-ffreestanding -nostdlib` 等 flag |
| CMake toolchain file | `CMAKE_SYSTEM_NAME Generic` 告诉 CMake 无目标 OS |
| MBR | BIOS 加载第一扇区到 `0x7C00`，512 字节，必须以 `0xAA55` 结尾 |
| VGA 文本模式 | 显存映射在 `0xB8000`，80×25，每字符 2 字节（字符+属性） |
| AT&T 汇编语法 | `src, dst` 顺序，寄存器 `%` 前缀，立即数 `$` 前缀 |
| objcopy | ELF → 裸二进制转换，`-O binary` 参数 |
| 测试框架 | `TEST`/`ASSERT_*` 宏 + 自动注册，Host/QEMU 双模式 |
