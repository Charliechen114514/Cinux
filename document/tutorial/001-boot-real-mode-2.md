# 从 MBR 到图形模式：Stage2 引导与 VESA 初始化

> 标签：x86, 实模式, Stage2, A20, VESA, VBE, 线性帧缓冲, CMake
> 前置：[从 BIOS 到 MBR](001-boot-real-mode-1.md)

## 前言

上一章我们让 MBR 成功加载了 Stage2 并跳转过去，QEMU 屏幕上能看到 `Cinux Booting...`。但 Stage2 此刻还是一片空白——这篇我们要让它做三件关键的事情：开启 A20 地址线突破 1MB 限制，通过 VESA BIOS Extensions 设置 1024×768×32bpp 的图形模式，最后把帧缓冲的物理地址和分辨率信息保存到固定位置，留给后续的内核初始化使用。同时还要搞定构建系统——CMakeLists.txt 从单目标变成双目标，build_image.sh 负责把 MBR 和 Stage2 组装成磁盘镜像。

完成之后，QEMU 会从 80×25 文本模式切换到 1024×768 的图形模式——屏幕会变黑，别慌，这是正常的。用 GDB 可以验证帧缓冲信息已经正确保存在 0x6400。

## 环境说明

和上一篇相同的实验环境：WSL2 Ubuntu，QEMU 7.0+，GNU AS + CMake。新增的依赖只有本地 Intel SDM PDF（用于查 VBE 相关章节）。验证方式从"看屏幕"变成了"用 GDB 查内存"——因为图形模式下文本输出失效了。

## 上号！Stage2 的入口和初始化

MBR 用 `ljmp $0x0800, $0` 跳转过来后 CS=0x0800。和 MBR 一样，第一件事是重新规范化所有段寄存器。打开 `boot/stage2.S`：

```asm
_start:
    cli
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw $0x900, %ax
    movw %ax, %ss
    movw $STAGE2_STACK_BASEADDR, %sp
    sti
```

你会发现这和 MBR 的初始化几乎一模一样——这不是冗余，而是防御性编程。MBR 到 Stage2 是一次远跳转，只修改了 CS 和 IP，其他段寄存器的值理论上还保持着 MBR 设的状态（都是 0），但我们不依赖前一个阶段的寄存器残留值。每个引导阶段都是自包含的，如果将来 MBR 的初始化逻辑改了，Stage2 不会因此坏掉。

SS 设为 0x0900 而不是沿用 CS——这样栈的物理地址是 0x0900 × 16 + 0xFFFE = 0x9FFFE，远离 Stage2 代码区域（0x8000 起）和 VESA 数据区（0x6000-0x6410）。SP=0xFFFE 是一个常见的实模式栈顶选择——x86 栈 push 先减 2 再写入，0xFFFE 减 2 得 0xFFFC，这是一个对齐地址。

初始化完成后打印确认消息，然后依次调用 A20 和 VESA 函数：

```asm
    movw $(msg_stage2_ok), %si
    call print_string
    call enable_a20
    call vesa_get_controller_info
    call vesa_get_mode_info
    movw $(msg_mode_info_ok), %si
    call print_string
    call vesa_set_mode
    call vesa_save_framebuffer_info
```

这里有一个重要的时序安排：所有 `print_string` 调用都在 `vesa_set_mode` 之前。原因很简单——设置视频模式后屏幕切换到图形模式，BIOS teletype 输出（INT 0x10 AH=0x0E）失效了。所以调试信息必须在切换前全部打印完，切换后只能靠 GDB 验证。

## 开启 A20——突破 1MB 的历史遗留问题

接下来问题来了：为什么要开启 A20？这事得从 8086 说起。8086 有 20 根地址线（A0–A19），能寻址 1MB。当程序访问超过 1MB 的地址时（比如 `0xFFFF:0x0010` = 0x100000），地址会回绕到 0（变成 0x00000）。有些早期程序依赖了这个回绕行为——OSDev Wiki 的 A20 Line 页面详细记录了这段历史。到了 286 时代 CPU 有了更多地址线，为了兼容，Intel 决定默认关闭第 21 根地址线（A20），让回绕行为保持不变。

Cinux 使用最安全的 BIOS 方式开启 A20，函数在 `boot/common/serial.S` 中：

```asm
enable_a20:
    push %ax
    push %ds
    push %es
    movw $0x2401, %ax
    int $0x15
    jc .a20_failed
    pop %es
    pop %ds
    pop %ax
    ret
.a20_failed:
    pop %es
    pop %ds
    pop %ax
    movw $(a20_error_msg), %si
    jmp panic
```

INT 0x15 AX=0x2401 是 BIOS 提供的 A20 开启接口。CF=0 成功，CF=1 失败。在 QEMU 中这个调用总是成功的——QEMU 的 Bochs BIOS 完全支持。但如果在真机上失败，可以回退到 Fast A20 方式（读写端口 0x92 的 bit 1），或者键盘控制器 8042 方式。失败后调用 panic 打印错误信息并停机——在 bootloader 阶段，A20 开不了意味着后续所有超过 1MB 的内存访问都会出问题，不如直接报错。

有些教程会建议直接操作端口 0x92 来开启 A20，确实更快——只有两条 I/O 指令。但端口 0x92 的 bit 0 连着 reset line，如果操作不当会触发系统重启。BIOS 方式虽然多一次中断调用的开销，但安全得多。

## VESA 三步曲——从文本世界进入图形世界

VESA BIOS Extensions（VBE）是显卡厂商和 VESA 组织定义的一套标准接口，让我们可以通过 BIOS 调用查询和设置高分辨率图形模式。OSDev Wiki 的 VBE 页面告诉我们，VBE 2.0+ 支持线性帧缓冲——显存被映射到一段连续物理地址，直接往那个地址写颜色值就能在屏幕上显示像素。不用像 VGA 文本模式那样操作端口、设光标位置。

Cinux 使用三步流程：先获取控制器信息确认 VBE 2.0+ 可用，再查询目标模式（0x118 = 1024×768×32bpp）的详细参数，最后设置模式并保存帧缓冲信息。

### 第一步：获取控制器信息

```asm
vesa_get_controller_info:
    push %ax
    push %es
    push %di
    movw $VBE_VBE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di
    movw $0x4256, %es:(%di)
    movw $0x3245, %es:2(%di)
    movw $0x4F00, %ax
    int $0x10
    cmpb $0x4F, %al
    jne .vesa_ctrl_failed
    cmpb $0, %ah
    jne .vesa_ctrl_failed
    pop %di
    pop %es
    pop %ax
    ret
```

我们在 0x6000 放一个 512 字节的 VbeInfoBlock 缓冲区。ES 段 = 0x6000 >> 4 = 0x600，DI=0，ES:DI 指向物理地址 0x6000。调用前在缓冲区头部写入 `"VBE2"` 签名——小端序拆成两个 word：0x4256 = 'V''B'，0x3245 = 'E''2'。这个签名告诉 BIOS 我们要 VBE 2.0+ 格式的信息，不写的话某些 BIOS 会返回 VBE 1.x 格式。

返回值检查分两步：AL=0x4F 说明显卡支持 VBE，AH=0 说明调用成功。两个条件缺一不可——AL=0x4F 但 AH=0x01 意味着功能支持但调用失败。

ES 段的计算这里容易搞错——0x6000 的段地址是 0x6000 / 16 = 0x600，不是 0x6000。如果你直接把 0x6000 写进 ES，实际指向的物理地址会是 0x60000，完全不对。

### 第二步：查询模式 0x118 的详细信息

```asm
vesa_get_mode_info:
    push %ax
    push %cx
    push %es
    push %di
    movw $VBE_MODE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di
    movw $0x4F01, %ax
    movw $0x0118, %cx
    int $0x10
    cmpb $0x4F, %al
    jne .vesa_mode_failed
    cmpb $0x0, %ah
    jne .vesa_mode_failed
    pop %di
    pop %es
    pop %cx
    pop %ax
    ret
```

ModeInfoBlock 是 256 字节，放在 0x6200。CX=0x0118 是纯模式号，不带标志位——设置模式时才加 0x4000（bit14 启用线性帧缓冲），查询信息时用纯模式号。调用成功后，BIOS 在这个 256 字节结构里填满参数，我们后续需要的关键字段是：PhysBasePtr（偏移 +0x28，4 字节，帧缓冲物理地址）、BytesPerScanLine（偏移 +0x10，2 字节，每行字节数 / pitch）、XResolution（偏移 +0x12）、YResolution（偏移 +0x14）。

### 第三步：设置模式并保存帧缓冲信息

```asm
vesa_set_mode:
    movw $0x4F02, %ax
    movw $VESA_TARGET_MODE, %bx
    int $0x10
    cmpb $0x4F, %al
    jne .vesa_set_failed
    cmpb $0, %ah
    jne .vesa_set_failed
    ret
```

BX = 0x4118 = 0x118 | 0x4000。bit14（0x4000）告诉 BIOS 使用线性帧缓冲模式而不是 banked 模式。线性帧缓冲意味着整个显存映射到一段连续物理地址——QEMU 里通常是 0xFD000000——直接往这个地址写像素值就能在屏幕上显示颜色。banked 模式需要频繁切换 64KB 的显存窗口，复杂得多。

调用成功后屏幕变黑——从文本模式切换到图形模式，之前打印的所有文字都消失了。这是正常的，真正的坑在后面。

### 保存帧缓冲信息——跨段数据搬运

```asm
vesa_save_framebuffer_info:
    push %ax
    push %bx
    push %di
    push %es
    push %gs
    movw $VBE_MODE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di
    movw $VBE_FB_INFO_BLOCK >> 4, %bx
    movw %bx, %gs
    movl %es:MODE_PHYS_BASE_PTR(%di), %eax
    movl %eax, %gs:FB_INFO_PHYS_ADDR(%di)
    movw %es:MODE_BYTES_PER_SCAN_LINE(%di), %ax
    movw %ax, %gs:FB_INFO_PITCH(%di)
    movw %es:MODE_X_RESOLUTION(%di), %ax
    movw %ax, %gs:FB_INFO_WIDTH(%di)
    movw %es:MODE_Y_RESOLUTION(%di), %ax
    movw %ax, %gs:FB_INFO_HEIGHT(%di)
    pop %gs
    pop %es
    pop %di
    pop %bx
    pop %ax
    ret
```

这是整个 common/serial.S 中最精巧的函数。它需要从 ModeInfoBlock（ES 段，0x6200）读数据，写到 FB Info（GS 段，0x6400），两个数据结构在不同的段里。`%es:` 和 `%gs:` 是段超越前缀——告诉 CPU 这次内存访问用哪个段寄存器，而不是默认的 DS。

代码里的注释 `CRITICAL FIX: Use segment override prefixes!` 说明这里踩过坑。如果不加段超越前缀，`movl MODE_PHYS_BASE_PTR(%di), %eax` 默认用 DS 段，但此时 DS=CS=0x0800，读到的就是 0x0800 × 16 + 0x28 = 0x08028 处的随机数据。这个 bug 非常隐蔽——如果 DS 碰巧和 ES 指向同一个段（比如都是 0），数据碰巧是对的，但 Stage2 里 DS=0x0800。

最终 0x6400 处保存了 16 字节的帧缓冲信息：

```
0x6400 +0  phys_addr (8B) ← QEMU 典型值 0xFD000000
       +8  pitch     (4B) ← 4096 (1024 × 4 bytes/pixel)
       +C  width     (2B) ← 1024
       +E  height    (2B) ← 768
```

后续内核的图形驱动会用这份数据初始化 framebuffer——像素坐标 (x, y) 对应的地址 = PhysBasePtr + y × pitch + x × 4。

### 设计对比：Cinux vs ToaruOS vs Linux

ToaruOS 的 bootloader 是教学 OS 中最复杂的——它使用 unreal mode 技巧（切到保护模式开 A20，再切回实模式但保留 32 位地址线），实现在保护模式和实模式之间来回切换以复用 BIOS 调用，还包含 ISO 9660 文件系统驱动和图形启动菜单。Cinux 的 VESA 初始化更简单：全程在实模式下通过 BIOS 调用完成，不需要 unreal mode 技巧。Linux 的 boot protocol 在视频模式设置上提供了更多选项——支持 VBE 模式、文本模式、甚至 EFI GOP，通过 boot_params 结构传递参数。Cinux 目前硬编码 0x118 模式，后续可以像 Linux 一样支持模式枚举和用户选择。

## 构建系统——从源码到磁盘镜像

现在我们回头看构建系统。tag 000 只有一个 mbr 目标，现在需要支持 MBR + Stage2 双目标。打开 `boot/CMakeLists.txt`：

关键的设计选择有三个。第一，common/serial.S 编译为 OBJECT 库（`boot_common`），被 stage2 引用但不被 mbr 引用——这是 512 字节限制的必然选择。第二，MBR 链接脚本起始地址 0x7C00（实际运行地址），Stage2 起始地址 0（段寄存器负责偏移）——两种不同的地址模型在同一个 CMakeLists 里共存。第三，两个目标都使用 `-Wa,--32` + `elf_i386`，在 x86_64 主机上交叉编译 16 位实模式代码。

链接脚本有一个细节值得注意：`.rodata` 被合并到 `.text` 段。这是因为 `objcopy -O binary` 只输出有内容的段，如果 `.rodata` 单独一个段，它在 ELF 里的位置可能在 `.text` 后面有空隙，objcopy 输出的二进制会包含这个空隙（用零填充），导致二进制尺寸膨胀。合并到 `.text` 里确保所有数据紧密排列。

`build_image.sh` 做三件事：创建 1MB 全零文件，写入 MBR 到扇区 0，写入 Stage2 从扇区 1 开始。`conv=notrunc` 不能少——没有它 dd 会截断输出文件。脚本还检查 MBR 签名和 Stage2 大小（不超过 15 扇区），以及打印调试信息让你确认每一步都正确。

## 验证

构建运行和上一篇一样：

```
cmake -B build && cmake --build build
qemu-system-x86_64 -drive format=raw,file=build/cinux.img
```

QEMU 屏幕应该依次显示 `Cinux Booting...`、`Stage2 OK`、`Mode info OK, switching...`，然后屏幕变黑——图形模式切换成功。如果前两条消息没出现就黑屏了，说明 Stage2 在 VESA 调用前就出了问题；如果 `Mode info OK` 出现后没有黑屏，说明 set_mode 调用失败了。

GDB 验证帧缓冲信息：

```
(gdb) target remote :1234
(gdb) set architecture i8086
(gdb) x/2gx 0x6400
0x6400: 0x00000000fd000000  ...
```

PhysBasePtr = 0xFD000000 就对了。想验证帧缓冲本身能写，可以用 GDB 画一个红色方块：

```
(gdb) set $y = 0
(gdb) while $y < 100
 > set $x = 0
 > while $x < 100
  > set {int}(0xfd000000 + ($y * 4096 + $x * 4)) = 0x00FF0000
  > set $x = $x + 1
 > end
 > set $y = $y + 1
(gdb) end
```

左上角出现红色方块——到这里就大功告成了，图形模式设置成功，帧缓冲可用。

## 收尾

这两章我们从 BIOS 加载 MBR 开始，一路走到了 1024×768×32bpp 的图形模式。完整的实模式引导链路已经打通：MBR 初始化段寄存器 → 读磁盘加载 Stage2 → 开启 A20 → VESA 三步初始化 → 保存帧缓冲信息。每个阶段的代码都在实模式下运行，使用段:偏移寻址和 BIOS 中断调用——这就是 x86 几十年遗产的全部重量。

下一篇我们要离开实模式了——设置 GDT，切换到保护模式，然后继续推进到 64 位长模式。真正的 x86 内核开发才刚刚开始。

## 参考资料

- Intel SDM: Vol.3A §10.1.1 — Processor State After Reset, A20 gate behavior
- Intel SDM: Vol.3A §21.1.1 — Real-Address Mode segment:offset addressing
- OSDev Wiki: [A20 Line](https://wiki.osdev.org/A20_Line) — A20 历史、开启方式、测试方法
- OSDev Wiki: [VESA Video Modes](https://wiki.osdev.org/VESA_Video_Modes) — VbeInfoBlock、ModeInfoBlock 字段定义
- ToaruOS: `boot/boot.S` — 使用 unreal mode 的复杂 bootloader 实现
- Linux: [x86 boot protocol](https://docs.kernel.org/arch/x86/boot.html) — 视频模式设置和 boot_params 结构
