# 从零搭建 x86_64 OS 开发环境（中）—— MBR 存根：第一个"能跑的东西"

> 标签：MBR, 实模式, VGA, 汇编, QEMU
> 前置：完成 000-1（工具链安装 + CMake 配置）

## 第三步——MBR 存根：从源码到 QEMU 启动

构建系统配好了，
现在我们来写第一个"能跑的东西"
——一个最小的 MBR 存根。
它的全部功能就是清屏、在左上角打印一个蓝底白字的 "C"，然后停机。
代码逻辑简单到不值一提，
但它验证了从汇编到 QEMU 启动的完整流水线，
这是后续所有工作的基础。

### MBR 是什么

当你按下电脑电源键的那一刻，
CPU 从一个固定的物理地址开始执行固件代码（BIOS 或 UEFI），
这段代码做完硬件自检（POST）之后，
会从启动盘的第一个扇区读取 512 字节数据，
加载到物理内存地址 `0x7C00`，然后把控制权交给它。
这个 512 字节就是 MBR（Master Boot Record），
它是整个操作系统启动链条的第一环。

为什么是 512 字节？因为硬盘的一个扇区就是 512 字节，
这是 IBM PC 时代留下来的约定。
为什么是 `0x7C00`？
这同样是 BIOS 规范定义的固定地址，
所有的 x86 BIOS 都会在这个地址加载 MBR，不会有例外。
MBR 的最后两个字节必须是 `0x55` 和 `0xAA`
（合在一起看是 `0xAA55`），
BIOS 会检查这个"魔数"，
如果不对就认为这不是一个有效的启动扇区，直接报错。

在 tag 000 里，我们的 MBR 只是一个"存根"：
清屏、打印一个字母、停机。
它的唯一目的是验证整个构建链条是通的。

### VGA 文本模式——最简单的屏幕输出

在还没有显卡驱动的时候，
怎么在屏幕上显示字符？
答案是 VGA 文本模式——
这是一种硬件级的文本显示机制，
由 VGA 兼容显卡直接支持，不需要安装任何驱动。
文本模式的显存被映射到物理地址 `0xB8000`，
你往这里写数据，屏幕上立刻就能看到对应的变化。
屏幕被分成 80 列 x 25 行共 2000 个字符单元，
每个单元占 2 字节：
第一个字节是字符的 ASCII 码，
第二个字节是显示属性。
比如属性值 `0x1F` 就是蓝底白字
（背景色 1=蓝色，前景色 F=亮白色）。

Wikipedia 的 VGA-compatible text mode 页面有详细的属性字节位域说明
——bit7 控制闪烁，bit6-4 是背景色（3 位，8 种），
bit3-0 是前景色（4 位，16 种）。

### 完整的 MBR 存根

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

.org 510
.word 0xAA55
```

`.code16` 告诉 GNU AS 生成 16 位实模式指令，
因为这段代码会被 BIOS 在 CPU 上电后的初始模式下执行。
如果你忘了加 `.code16`，
汇编器会生成 32 位指令，CPU 根本不认识，
直接 triple fault——而且 QEMU 的报错信息完全不会告诉你"指令模式不对"，
它只会默默地重启或者黑屏。这种 bug 真的很让人抓狂。

清屏部分把 ES:DI 指向 VGA 文本缓冲区的物理地址 `0xB8000`
（实模式下 ES=`0xB800`，左移 4 位就是 `0xB8000`），
然后用 `rep stosw` 把 2000 个字符单元（80列 x 25行）
全部填成灰色空格。
`0x0720` 是属性字节和字符的组合：
低字节 `0x20` 是空格，高字节 `0x07` 是灰底黑字。

打印字符的部分非常直接：
把属性 `0x1F`（蓝底白字）放到 AH，
字符 'C' 放到 AL，然后写入 ES:0
——屏幕左上角立刻出现一个蓝底白字的 C。
这里我们直接写 VGA 显存而不是调 BIOS 中断，
因为直接写显存更快，而且在 QEMU 里完全可用。
这种做法在真实的 BIOS 环境下也是标准的
——Linux 内核的早期启动代码（`arch/x86/boot/` 目录）
同样直接写 `0xB8000`。

`.org 510` 跳转到偏移 510 的位置，
然后 `.word 0xAA55` 写入签名。
在小端序下实际存储为字节 `0x55, 0xAA`。
BIOS 会检查扇区的最后两个字节是否为 `0x55 0xAA`（磁盘上的字节序），
如果不是就拒绝执行。
这个签名是 IBM PC 兼容机的硬性约定，
从 1981 年的第一台 PC 到现在的 QEMU 模拟器，没有例外。

### 编译、链接、转换

MBR 的编译流水线需要特别处理，
因为它不是普通的可执行文件
——它是被 BIOS 加载到固定物理地址执行的裸二进制。
`boot/CMakeLists.txt` 里定义了这个流程：

```cmake
add_executable(mbr mbr.S)

target_compile_options(mbr PRIVATE
    -Wa,--32
)

target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)

add_custom_command(
    TARGET mbr POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    VERBATIM
)
```

三步走：AS 汇编 -> ld 链接 -> objcopy 转裸二进制。
`-Wa,--32` 让汇编器生成 32 位目标文件
——16 位代码（`.code16`）在 32 位 ELF 里编码兼容性最好。
`-Wl,-m,elf_i386` 指定链接器使用 32 位 i386 ELF 格式。
`-no-pie` 禁用位置无关可执行文件
——MBR 运行在固定地址 `0x7C00`，不需要也不允许 PIC 重定位。

链接脚本在 CMake configure 时生成，
设置了 `OUTPUT_FORMAT("elf32-i386")`、
入口地址为 `0x7C00`。
`objcopy -O binary` 是关键一步
——它把 ELF 可执行文件（带文件头、段头表、符号表等元数据）
转换成纯粹的二进制字节流，
因为 BIOS 只认原始字节，ELF 格式的文件头对它来说就是垃圾数据。

最后一步是用 `dd` 把 MBR 裸二进制写入磁盘镜像的第一个扇区，
然后交给 QEMU 启动。
整套流程自动化在 `scripts/build_image.sh` 和 `cmake/qemu.cmake` 里，
所以你只需要：

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make run
```

QEMU 窗口弹出，左上角蓝底白字的 "C" 出现，CPU 停机。
到这里我们就验证了从源码到运行的全部构建链。

### 踩坑：构建流水线中容易出错的地方

整个编译流水线有几处特别容易踩坑的地方，值得单独拎出来说。

第一，链接脚本的输出格式必须是 `elf32-i386` 而不是 `elf64-x86-64`。
虽然 Cinux 的最终目标是 64 位内核，
但 MBR 运行在 16 位实模式下，
用 32 位 ELF 格式链接是标准做法
——`.code16` 指令告诉汇编器生成 16 位操作码，
而这些操作码在 32 位目标文件里编码兼容性最好。
如果用了 64 位 ELF 格式，
虽然汇编能通过，但某些指令编码会出错。

第二，`objcopy -O binary` 的行为是基于链接脚本的 LMA（Load Memory Address）的。
如果链接脚本里 `. = 0x7C00`，
objcopy 会在输出文件开头填充 0x7C00 个字节的零，
然后才是实际的代码——
导致输出文件远大于 512 字节。
Cinux 的链接脚本里只有一个 `.text` 段且起始地址就是 `0x7C00`，
所以 objcopy 的输出恰好是代码的实际内容，不会有多余的填充。

第三，`dd` 命令的 `conv=notrunc` 参数千万不能漏。
没有它的话，`dd` 在写入完成后会把输出文件截断到写入数据的长度。
tag 000 只写入 MBR 所以看不出来，
但后续 tag 写入 stage2 和内核时，
镜像会被截回 512 字节，
QEMU 读到一堆零直接崩溃——
而且这个 bug 的表现完全不指向 dd，
你会花很多时间去排查汇编代码和链接脚本。

### 调试 MBR 的技巧

如果 QEMU 启动后黑屏，排查步骤：

先用 `xxd build/boot/mbr.bin | tail -1` 检查最后两个字节是不是 `55aa`。
如果不是，签名写错了或者文件大小不对。
如果签名正确但仍然黑屏，
用 `xxd build/cinux.img | head -20` 检查镜像前几十个字节，
确认 MBR 内容确实被写入了第 0 扇区。
如果镜像全是零，说明 `build_image.sh` 的路径参数不对。

还可以用 GDB 远程调试 MBR：

```bash
# 终端 1：启动 QEMU（暂停等待 GDB）
qemu-system-x86_64 -S -gdb tcp::1234 -drive format=raw,file=build/cinux.img

# 终端 2：连接 GDB
gdb
  (gdb) target remote :1234
  (gdb) set architecture i8086
  (gdb) break *0x7c00
  (gdb) continue
```

在 0x7C00 设断点，单步执行，
观察段寄存器和内存的变化。
注意实模式下 GDB 显示的地址需要手动计算 `CS x 16 + IP`。

下一篇我们将搭建自研测试框架，
为后续的内核开发提供持续验证的能力。

## 参考资料

- Intel SDM — [Volume 1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  Chapter 3 Basic Execution Environment，描述实模式寄存器和内存组织
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式和 BIOS 引导行为
- OSDev Wiki — [Real Mode](https://wiki.osdev.org/Real_Mode): 实模式寻址机制
- Wikipedia — [VGA-compatible text mode](https://en.wikipedia.org/wiki/VGA-compatible_text_mode):
  VGA 文本缓冲区地址 0xB8000 和属性字节格式
