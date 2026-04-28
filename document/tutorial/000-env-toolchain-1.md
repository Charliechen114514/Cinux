# 从零搭建 x86_64 OS 开发环境

> 标签：OS开发, CMake, 裸机编译, MBR, QEMU, 工具链
> 前置：熟悉 Linux 命令行，有基本的 C/C++ 经验

## 前言：为什么环境搭建能劝退一半的人

如果你跟我一样，第一次决定"我要自己写一个操作系统"的时候，大概率是在某个深夜被一篇 OS 开发教程给戳中了。然后你兴冲冲地打开终端准备大干一场，第一步就卡住了——教程说"请先编译一个交叉编译器"，然后丢给你一个 2006 年的 gcc 源码链接，让你 `make` 两个小时。等你真的把交叉编译器编出来（或者没编出来放弃了），写了两行代码编译的时候发现链接报错、QEMU 黑屏、教程用的 NASM 而你想用 GAS，整套流程下来你已经不想写 OS 了，你想把电脑砸了。

这就是为什么 Cinux 的第一个 tag 什么都不做，专门用来搭环境。我们要做的事情听起来很无聊——装几个包、写个检查脚本、配一下 CMake、编译一个只会在屏幕上显示字母 "C" 然后停机的 512 字节程序。但这一步解决了后续所有 tag 都会依赖的构建基础设施：从源码到可启动磁盘镜像的完整流水线。现在把它搭好、搞懂每一行配置的含义，后面写 bootloader、内核、驱动的时候就不会再因为编译问题卡住了。

## 我们的环境

实验环境是一台 x86_64 Linux 机器（Ubuntu 22.04 或更新版本），工具链完全来自系统包管理器——不需要编译交叉编译器，不需要 Docker，不需要任何特殊准备。GCC 版本要求 11+（支持 C++23 的基本特性），CMake 笔者使用的是 4.1（实际 3.20 以上即可），QEMU 要求 8.0+（模拟 x86_64 启动）。编辑器随意，但如果用 VS Code 的话，项目里已经配好了 `.clangd` 文件，装上 clangd 插件就能获得代码补全和错误提示。

这里有一个值得展开说说的选择：为什么不用交叉编译器？OSDev Wiki 上的 Bare Bones 教程强烈建议你构建一个 `x86_64-elf-gcc` 交叉编译器，理由是系统 GCC 可能会隐式链接宿主系统的库、假设宿主系统的 ABI、甚至生成一些在裸机上无法运行的代码。这些担忧在理论上是成立的，但在实际操作中，只要你加上 `-ffreestanding -nostdlib -static` 这些 flag，系统 GCC 生成的代码在 x86_64 裸机上就是完全可用的——因为我们的宿主机本身就是 x86_64，指令集层面没有任何差异。真正的交叉编译场景（比如在 x86_64 宿主上编译 ARM 内核）确实需要专用工具链，但 x86_64 → x86_64 这种"伪交叉编译"，系统 GCC 完全胜任。

对比一下其他项目的做法：xv6 用的是最暴力的方案——Makefile 里直接 `CC = $(TOOLPREFIX)gcc`，flag 硬编码在 `CFLAGS` 里，没有任何 toolchain file 的概念。这种做法在小项目里完全够用，但当源文件从 5 个增长到 50 个的时候，Makefile 就开始变成意大利面条了。SerenityOS 则走另一个极端，它的 `Meta/CMake/` 目录下有 30 多个 CMake 模块，支持多编译器（GCC/Clang）、多架构（x86_64/AArch64/RISC-V）、代码生成器、IPC IDL 编译器等，复杂度堪比一个中型框架。Cinux 选择了一条中间路线：用 CMake 和 toolchain file 管理，但保持最小化的配置——刚好够用，不多不少。

## 第一步——安装工具链并验证

首先把需要的包装上：

```bash
sudo apt install -y gcc g++ binutils qemu-system-x86 cmake gcc-multilib g++-multilib
```

`gcc-multilib` 和 `g++-multilib` 这两个包非常容易遗漏，但它们是必须的——Cinux 的 MBR 和 stage2 bootloader 需要 32 位 ELF 链接（`-m elf_i386`），即使最终的 64 位内核不需要。如果你漏装了 multilib 包，链接 bootloader 的时候会收到一个诡异的 `cannot find -lgcc_s` 错误，而且报错信息完全不指向真正的原因，这个坑真的坑了笔者半天。

装完之后跑一下检查脚本确认：

```bash
./scripts/check_toolchain.sh
```

你应该看到每个工具后面都跟着绿色的 `[OK]`，最后一行是 `[OK] All required tools are installed!`。如果任何一个工具缺失，脚本会直接退出并告诉你安装命令。

## 第二步——配 CMake 构建系统

接下来我们要配 CMake，这是整个项目的构建骨架。核心文件有三个：顶层 `CMakeLists.txt`、`cmake/toolchain-x86_64.cmake`、和 `cmake/qemu.cmake`。我们从 toolchain file 开始看，因为它是整个编译配置的灵魂。

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_FLAGS_INIT "
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
    -mcmodel=kernel
    -Wall
    -Wextra
")

set(CMAKE_CXX_FLAGS_INIT ${CMAKE_C_FLAGS_INIT} "
    -fno-exceptions
    -fno-rtti
    -std=c++23
")

set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib                
    -static
    -Wl,--build-id=none      
")
```

`CMAKE_SYSTEM_NAME Generic` 是这整个文件最重要的一个设置——它告诉 CMake "目标平台没有操作系统"。这个值会触发 CMake 的一系列行为变化：禁用 `find_package` 的默认搜索（因为没有系统库可找），不允许 `try_run`（因为编译产物不能在宿主上运行），等等。笔者有一次手滑把这个写成了 `Linux`，结果后面所有的编译都正常通过，但链接出来的内核一跑就 triple fault——因为 CMake 按照 Linux 程序的方式注入了一堆不该有的链接 flag。排查了整整两个小时才发现是这一个词的问题，那种感觉真的让人血压拉满。

编译 flag 的含义我们逐个过一遍。`-ffreestanding` 告诉 GCC 代码运行在没有操作系统的裸机环境，只提供 freestanding 头文件（`<stdint.h>`, `<stddef.h>`, `<stdarg.h>` 等），不提供 `printf`、`malloc` 这些。`-fno-stack-protector` 禁用栈保护，因为栈保护的 canary 初始化依赖 `__stack_chk_fail` 函数，这个函数需要标准库提供，内核里什么都没有。`-mno-red-zone` 禁用 x86_64 System V ABI 里的 red zone——这是给用户态程序的一个 128 字节栈下空间优化，允许函数在不调整 RSP 的情况下使用栈空间。但在内核里，中断可能在任何时候打断执行（包括红区里的代码），如果用了红区，中断处理程序的栈帧会覆盖掉还没用完的数据，直接导致数据损坏和难以调试的随机崩溃。Intel SDM Volume 1 Chapter 3 对此有明确说明：内核代码必须假设任何指令都可能被中断。`-mcmodel=kernel` 告诉编译器代码会运行在地址空间最顶端（高半核，约 `0xFFFFFFFF80000000`），这影响了它生成绝对地址引用的方式。

C++ 特有的 flag 禁用了异常和 RTTI。为什么 OS 内核不能用异常？原因在于异常的展开（unwinding）需要 `.eh_frame` 段和 `__cxa_begin_catch`、`__cxa_end_catch` 等运行时函数的支持，这些都需要标准库提供。而且异常展开会隐式调用析构函数，这要求堆分配器已经初始化——但堆分配器本身就是内核的一部分，形成了循环依赖。SerenityOS 的做法更有趣：它允许异常，但自己实现了全套异常运行时（`__cxa_*` 函数），代价是显著的复杂度。对于教学项目来说，直接禁用异常用返回值替代是更简洁的选择。

`CMAKE_*_FLAGS_INIT` 和直接设 `CMAKE_*_FLAGS` 的区别值得说一句。`_INIT` 变量只在第一次 configure 时被读取，之后用户可以通过命令行追加自定义 flag 而不会和 toolchain file 的 flag 冲突；直接设 `CMAKE_C_FLAGS` 则会阻止用户的 flag 生效。所以 toolchain file 里必须用 `_INIT` 版本，这是 CMake 官方推荐的做法。

## 第三步——MBR 存根：从源码到 QEMU 启动

构建系统配好了，现在我们来写第一个"能跑的东西"——一个最小的 MBR 存根。它的全部功能就是清屏、在左上角打印一个蓝底白字的 "C"，然后停机。代码逻辑简单到不值一提，但它验证了从汇编到 QEMU 启动的完整流水线，这是后续所有工作的基础。

```asm
.section .text
.code16
.global _start

_start:
    mov $0xB800, %ax
    mov %ax, %es
    xor %di, %di
    mov $0x0720, %ax
    mov $2000, %cx
    cld
    rep stosw

    mov $0x1F, %ah
    mov $'C', %al
    mov %ax, %es:(0)

    cli
    hlt
    jmp _start

.fill 510 - (. - _start), 1, 0
.word 0xAA55
```

`.code16` 告诉 GNU AS 生成 16 位实模式指令，因为这段代码会被 BIOS 在 CPU 上电后的初始模式下执行。如果你忘了加 `.code16`，汇编器会生成 32 位指令，CPU 根本不认识，直接 triple fault——而且 QEMU 的报错信息完全不会告诉你"指令模式不对"，它只会默默地重启或者黑屏。这种 bug 真的很让人抓狂。

清屏部分把 ES:DI 指向 VGA 文本缓冲区的物理地址 `0xB8000`（实模式下 ES=`0xB800`，左移 4 位就是 `0xB8000`），然后用 `rep stosw` 把 2000 个字符单元（80列 × 25行）全部填成灰色空格。`0x0720` 是属性字节和字符的组合：低字节 `0x20` 是空格，高字节 `0x07` 是灰底黑字。Wikipedia 的 [VGA-compatible text mode](https://en.wikipedia.org/wiki/VGA-compatible_text_mode) 页面有详细的属性字节位域说明——bit7 控制闪烁，bit6-4 是背景色（3 位，8 种），bit3-0 是前景色（4 位，16 种）。

打印字符的部分非常直接：把属性 `0x1F`（蓝底白字）放到 AH，字符 'C' 放到 AL，然后写入 ES:0——屏幕左上角立刻出现一个蓝底白字的 C。这里我们直接写 VGA 显存而不是调 BIOS 中断，因为直接写显存更快，而且在 QEMU 里完全可用。这种做法在真实的 BIOS 环境下也是标准的——Linux 内核的早期启动代码（`arch/x86/boot/` 目录）同样直接写 `0xB8000`。

`.fill` 和 `.word 0xAA55` 确保 MBR 恰好 512 字节且以有效签名结尾。BIOS 会检查扇区的最后两个字节是否为 `0x55 0xAA`（磁盘上的字节序），如果不是就拒绝执行。这个签名是 IBM PC 兼容机的硬性约定，从 1981 年的第一台 PC 到现在的 QEMU 模拟器，没有例外。

### 编译、链接、转换

MBR 的编译流水线需要特别处理，因为它不是普通的可执行文件——它是被 BIOS 加载到固定物理地址执行的裸二进制。`boot/CMakeLists.txt` 里定义了这个流程：

```cmake
add_executable(mbr mbr_stub.S)

target_link_options(mbr PRIVATE
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
)

add_custom_command(
    TARGET mbr POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    VERBATIM
)
```

三步走：AS 汇编 → ld 链接 → objcopy 转裸二进制。链接脚本在 CMake configure 时生成，设置了入口地址为 `0x7C00`。`objcopy -O binary` 是关键一步——它把 ELF 可执行文件（带文件头、段头表、符号表等元数据）转换成纯粹的二进制字节流，因为 BIOS 只认原始字节，ELF 格式的文件头对它来说就是垃圾数据。

最后一步是用 `dd` 把 MBR 裸二进制写入磁盘镜像的第一个扇区，然后交给 QEMU 启动。整套流程自动化在 `scripts/build_image.sh` 和 `cmake/qemu.cmake` 里，所以你只需要：

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make run
```

QEMU 窗口弹出，左上角蓝底白字的 "C" 出现，CPU 停机。到这里我们就验证了从源码到运行的全部构建链。

## 第四步——测试框架：给内核一双眼睛

写 OS 内核没有测试是非常恐怖的事情——你改了一行代码，内核跑起来直接 triple fault，但你完全不知道是哪一行改坏了，因为内核里连 `printf` 都没有。Cinux 从第一个 tag 就引入了自研的轻量测试框架，支持 Host 模式（在 Linux 上用 g++ 编译运行）和 QEMU 模式（在内核里通过串口输出），让逻辑性的 bug 在 Host 端就能被捕获。

```cpp
#define TEST(test_name)                                                                            \
    static void _TEST_CAT(_test_fn_, __LINE__)();                                                  \
    static struct _TEST_CAT(_TestAutoReg_, __LINE__) {                                             \
        _TEST_CAT(_TestAutoReg_, __LINE__)() {                                                     \
            _register_test(test_name, __FILE__, __LINE__, _TEST_CAT(_test_fn_, __LINE__));         \
        }                                                                                          \
    } _TEST_CAT(_test_reg_, __LINE__);                                                             \
    static void _TEST_CAT(_test_fn_, __LINE__)()
```

`TEST("name") { ... }` 宏的设计借鉴了 Google Test 的自动注册机制——利用 C++ 静态对象的构造函数在 `main` 之前执行的特性，每个 `TEST` 块会自动把自己的函数指针注册到全局数组里，用户不需要手动维护测试列表。`_TEST_CAT` 两层间接展开解决了 `__LINE__` 和标识符拼接的经典预处理陷阱——直接 `##_LINE__` 不会展开 `__LINE__`，必须先通过一层宏让它展开成数字，再通过第二层做拼接。

对比其他项目的测试策略：xv6 完全没有测试框架，所有验证都靠手动运行和目测输出。这在只有几百行代码的教学项目里还行，但 Cinux 计划发展到几千行代码的时候，没有测试就太危险了。Linux 内核有 KUnit 子系统（在内核里跑单元测试）和 kselftest（用户态测试），但它们依赖内核已经能跑起来这一前提。Cinux 的双模式方案（Host + QEMU）的巧妙之处在于：Host 模式不需要内核就能测试纯逻辑（比如 kprintf 的格式化、GDT 描述符编码、内存管理算法），QEMU 模式则测试需要硬件交互的部分（比如中断处理、驱动初始化）。

跑一下冒烟测试验证框架工作正常：

```bash
make test
```

你应该看到 `=== Cinux Test Runner ===`，三个测试都显示 `[PASS]`，最后是 `Results: 3 passed, 0 failed` 和 `[SUITE PASSED]`。

## 到这里就大功告成了

回顾一下这一章做的事情：安装了工具链，配好了 CMake 构建系统（toolchain file + QEMU 集成），写了 MBR 存根并在 QEMU 里看到了第一个字符输出，还搭好了自研测试框架。这些都是后续所有 tag 的基础——现在偷懒跳过去，后面一定会还回来。

下一章我们将真正进入 MBR bootloader 的开发：BIOS 中断、磁盘读取、stage2 加载。准备好进入实模式的世界吧。

## 参考资料

- Intel SDM — [Volume 1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Chapter 3 Basic Execution Environment，描述实模式寄存器和 red zone 约束
- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones): 交叉编译器和 freestanding 编译 flag 的权威参考
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式和 BIOS 引导行为
- OSDev Wiki — [Real Mode](https://wiki.osdev.org/Real_Mode): 实模式寻址机制（已实际抓取验证）
- Wikipedia — [VGA-compatible text mode](https://en.wikipedia.org/wiki/VGA-compatible_text_mode): VGA 文本缓冲区地址 0xB8000 和属性字节格式（已实际抓取验证）
- CMake 官方 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html): toolchain file 完整文档
- xv6 — [mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv): 极简 Makefile 构建方案
- SerenityOS — [SerenityOS/serenity](https://github.com/SerenityOS/serenity): 生产级 OS 的 CMake 架构
