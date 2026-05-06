# 001 Read-through (2/3) —— Stage2 与共享函数库：A20 与 VESA 初始化

## 概览

本文讲解 `boot/stage2.S` 和 `boot/common/serial.S`。Stage2 是 MBR 加载的第二阶段引导程序，空间限制放宽到 7.5KB，可以链接外部函数库。它负责完成 MBR 做不了的事情：开启 A20 地址线、获取和设置 VESA 图形模式、保存帧缓冲信息供后续内核使用。

common/serial.S 是引导阶段的共享函数库，包含 print_string、panic、enable_a20 和四个 VESA 函数。它使用 `.code16gcc` 指令（而非 `.code16`），这是因为该文件需要和 stage2.S 一起被 `-Wa,--32` 编译成 32 位目标文件，`.code16gcc` 在这种模式下能正确生成 16 位代码。

关键设计决策：common/serial.S 只放函数不放数据（错误消息字符串除外），调用者（stage2.S）负责提供要打印的消息。这种"数据放调用者，函数放库"的模式是实模式多文件链接中的最佳实践——避免了跨文件符号引用在 16 位重定位下的各种问题。

## 架构图

```
Stage2 调用链：

_start (stage2.S)
  ├─ 初始化段寄存器 + 栈 (SS=0x900, SP=0xFFFE)
  ├─ print_string("Stage2 OK")
  ├─ enable_a20()                    ← INT 0x15 AX=0x2401
  ├─ vesa_get_controller_info()      ← INT 0x10 AX=0x4F00 → 0x6000
  ├─ vesa_get_mode_info()            ← INT 0x10 AX=0x4F01 → 0x6200
  ├─ print_string("Mode info OK")
  ├─ vesa_set_mode()                 ← INT 0x10 AX=0x4F02, BX=0x4118
  ├─ vesa_save_framebuffer_info()    ← 0x6200 → 0x6400
  └─ cli; hlt

内存布局（VESA 相关）：
  0x6000 ┌──────────────────┐
         │ VbeInfoBlock     │ 512B (控制器信息)
  0x6200 ├──────────────────┤
         │ ModeInfoBlock    │ 256B (模式信息)
  0x6400 ├──────────────────┤
         │ FB Info          │  16B (帧缓冲信息)
         │  +0  phys_addr   │  8B
         │  +8  pitch       │  4B
         │  +C  width       │  2B
         │  +E  height      │  2B
  0x6410 └──────────────────┘
```

## 代码精讲

### Stage2 入口——重新初始化一切

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

MBR 用 `ljmp $0x0800, $0` 跳转过来后 CS=0x0800。和 MBR 一样，第一件事是让所有段寄存器统一到 CS。但 SS 需要单独处理——我们不用 CS 的值（0x0800），而是手动设 SS=0x0900。这样栈的物理地址是 0x0900 × 16 + 0xFFFE = 0x9FFFE，远离 Stage2 代码区域（0x8000-0x9DFF）和 VESA 数据区（0x6000-0x6410）。SP 设为 0xFFFE（而非 0xFFFF）是因为 x86 栈 push 先减 2 再写入，0xFFFE 减 2 得 0xFFFC，这是一个对齐地址。

你可能会问：为什么要重新初始化段寄存器？MBR 不是已经设过了吗？因为 MBR 到 Stage2 是一次远跳转，跳转过程中只修改了 CS 和 IP，DS、ES 等其他段寄存器保持着 MBR 里的值（都是 0）。技术上说这一步是冗余的，但显式重新初始化是防御性编程——不依赖前一个阶段的寄存器状态，每个引导阶段都是自包含的。

### Stage2 主流程——按序调用

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

整个流程是严格线性的，每一步都依赖前一步的成功。`print_string` 在设置模式之前被调用两次——这是刻意的，因为 `vesa_set_mode` 之后文本输出就失效了（屏幕切换到图形模式，没有文本光标了）。`enable_a20` 在 VESA 操作之前调用，虽然当前的 VESA 操作不需要访问超过 1MB 的内存，但这是正确的初始化顺序——先让地址线就位，再进行后续操作。

末尾的 halt 循环是临时的——后续 tag 会在这里继续添加代码（进入保护模式、加载内核等）。

### print_string——保存寄存器的完整版

```asm
print_string:
    push %ax
    push %bx
    push %si
    cld
.loop:
    lodsb
    test %al, %al
    jz .done
    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    jmp .loop
.done:
    pop %si
    pop %ax
    pop %bx
    ret
```

和 MBR 的精简版相比，这个版本保存并恢复了 AX、BX、SI。这是因为在 Stage2 的流程中，调用 `print_string` 前后可能还需要这些寄存器的值——比如 SI 指向的消息地址在调用后应该保持不变。`xor %bx, %bx` 确保 BH=0（页号 0）和 BL=0（默认颜色），这不是多余的——BIOS 前一次调用可能修改了 BX，不清理的话字符可能输出到错误的页面。

### enable_a20——最安全的开启方式

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

INT 0x15 AX=0x2401 是 BIOS 提供的 A20 开启接口。CF=0 成功，CF=1 失败。失败路径和成功路径分别做 pop——这是因为 push 了三个寄存器，失败时不能直接 ret（栈不平衡），必须手动弹出。失败后跳转到 panic（而不是 call），因为 panic 永不返回，用 call 反而会浪费栈空间。

OSDev Wiki 记录了四种 A20 开启方式：BIOS INT 0x15（最安全）、Fast A20（端口 0x92）、键盘控制器 8042（最复杂）、端口 0xEE（某些 BIOS 特有）。Cinux 只用 BIOS 方式——在 QEMU 中总是有效，真机上如果不行可以加回退逻辑。

### vesa_get_controller_info——写入 VBE2 签名并查询

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

ES 段的计算：`0x6000 >> 4` = `0x600`。加上 DI=0，ES:DI 指向物理地址 0x6000。调用前在缓冲区头部写入 4 字节签名 `"VBE2"`（小端序拆成两个 word：0x4256 = 'V''B'，0x3245 = 'E''2'）。这个签名告诉 BIOS 我们想要 VBE 2.0+ 格式的信息——如果不写，某些 BIOS 会返回 VBE 1.x 格式，字段布局不同。

返回值检查分两步：AL=0x4F 表示这个 VBE 函数被支持（AL 其他值说明显卡根本不支持 VBE），AH=0 表示调用成功。这两个条件缺一不可——AL=0x4F 但 AH=0x01 意味着函数支持但调用失败了。

### vesa_get_mode_info——查目标模式的具体参数

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

CX=0x0118 是纯模式号（1024×768×32bpp），不带任何标志位。和 `vesa_set_mode` 里的 0x4118 区别在于：查询模式信息用纯模式号，设置模式时才加上 0x4000（bit14，启用线性帧缓冲）。ES:DI 指向 0x6200 的 ModeInfoBlock 缓冲区——调用成功后，BIOS 会在这个 256 字节的结构里填满模式参数。

### vesa_set_mode——切换到图形世界

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

`VESA_TARGET_MODE` = 0x4118 = 0x118 | 0x4000。bit14（0x4000）是关键——它告诉 BIOS 使用线性帧缓冲模式而不是 banked 模式。线性帧缓冲意味着整个显存被映射到一段连续物理地址，直接往那个地址写像素就行；banked 模式需要频繁切换 64KB 的显存窗口，复杂得多。调用成功后屏幕会从文本模式切换到图形模式——之前打印的所有文字都消失了，光标也不见了。

注意这个函数没有 push/pop——它不保存 AX 和 BX，调用者也不依赖这两个寄存器的值。这是故意精简的：每次调用这个函数都是"用完就扔"的场景，没有调用后还需要 AX/BX 的情况。

### vesa_save_framebuffer_info——跨段拷贝关键数据

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

这是整个 common/serial.S 中最精巧的函数。它需要从 ModeInfoBlock（0x6200）读数据，写到 FB Info（0x6400），两个数据结构在不同的段。ES 指向 0x6200（ModeInfoBlock），GS 指向 0x6400（FB Info），通过段超越前缀 `%es:` 和 `%gs:` 区分读写目标。

代码里的注释 `CRITICAL FIX: Use segment override prefixes!` 说明这是踩过坑的——如果不加段超越前缀，`movl MODE_PHYS_BASE_PTR(%di), %eax` 默认使用 DS 段，读到的就不是 ModeInfoBlock 的数据而是 DS 段里同一偏移位置的垃圾。这个 bug 非常隐蔽：如果 DS 碰巧和 ES 指向同一个段（比如都是 0），数据碰巧是对的；但 Stage2 里 DS=CS=0x0800，读到的就是 0x0800 × 16 + 0x28 = 0x08028 处的随机数据。

PhysBasePtr 用 `movl`（32 位）读，写入也是 32 位——但 FB Info 的物理地址字段是 8 字节，这里只写了低 4 位。在 QEMU 的 32 位物理地址空间里这就够了（0xFD000000 高 4 字节全是 0），但如果要支持 64 位物理地址需要额外写高 4 字节。pitch 用 `%ax`（16 位）中转——pitch 是 2 字节字段，而 FB Info 里 pitch 字段是 4 字节，高 2 字节自然为 0（4096 的 16 位表示已经足够）。

## 设计决策

### 决策：共享函数用 `.code16gcc` 而非 `.code16`

**问题**：common/serial.S 需要和 stage2.S 一起被 `-Wa,--32` 编译。

**本项目的做法**：使用 `.code16gcc`。

**备选方案**：使用 `.code16`。

**为什么不选备选方案**：`.code16` 生成的是纯 16 位目标文件格式，和 `--32` 编译选项不完全兼容。`.code16gcc` 专门设计为"在 32 位汇编模式下生成 16 位代码"——它生成的指令在 32 位目标文件里正确编码，但运行时执行的是 16 位实模式语义。这个细微差别是 GNU AS 在 16 位 + 32 位混合场景下的坑。

### 决策：VESA 函数内部处理错误（panic 而非返回错误码）

**问题**：VESA 调用可能失败，如何处理？

**本项目的做法**：每个 VESA 函数检查返回值，失败直接调用 panic 停机。

**备选方案**：返回错误码让 Stage2 决定是否继续。

**为什么不选备选方案**：在 bootloader 阶段，VESA 失败意味着没有图形模式可用，后续内核的 GUI 无法工作。与其默默继续然后在更奇怪的地方崩溃，不如立即打印明确的错误信息停机。这是 bootloader 中"快速失败"的设计哲学。

## 扩展方向

- **添加 VBE 模式枚举**（难度：⭐⭐）——不硬编码 0x118，而是遍历 VbeInfoBlock 里的模式列表，自动找到最佳模式。
- **添加 Fast A20 回退**（难度：⭐）——如果 BIOS 方式失败，尝试端口 0x92 的 Fast A20 方式。
- **帧缓冲测试图案**（难度：⭐）——在设置模式后、halt 之前，往帧缓冲写一个渐变色测试图案，验证图形模式确实生效。
- **支持 64 位 PhysBasePtr**（难度：⭐）——当前只保存了 PhysBasePtr 的低 32 位，理论上应该保存完整的 64 位地址。

## 参考资料

- Intel SDM: Vol.3A §10.1.1 — A20 gate behavior after processor reset
- OSDev Wiki: [A20 Line](https://wiki.osdev.org/A20_Line) — A20 history, enabling methods, testing
- OSDev Wiki: [VESA Video Modes](https://wiki.osdev.org/VESA_Video_Modes) — VbeInfoBlock, ModeInfoBlock, Set Mode flags
- OSDev Wiki: [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) — DAP structure layout
