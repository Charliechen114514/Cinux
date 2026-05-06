# 002-1 · 代码修正与 GDT 定义

## 概览

在上一篇里，我们让 stage2 在实模式下完成了全部 VESA 图形模式初始化——A20 开启、VBE 模式查询、帧缓冲信息保存，一路通畅。但实模式终究是个笼子：1MB 地址上限、没有权限检查、段地址偏移的寻址方式在现代操作系统里完全不可用。接下来我们要做的，是进入 x86 保护模式（Protected Mode）的第一步——把工程里几处代码修正到位，然后定义一张 GDT（全局描述符表）。

本文覆盖四个文件的变更：`serial.S` 的汇编模式修正（`.code16gcc` 改 `.code16`、`push/pop` 改 `pushw/popw`），`boot/CMakeLists.txt` 的 linker script 修正（base 从 `0x0` 改 `0x8000`、新增 `.gdt` section），`cmake/qemu.cmake` 新增 debugcon 支持，以及 `stage2.S` 中最核心的 GDT 定义。这些看起来都是"小修小补"，但说实话，每一个不动都会在后续的保护模式切换中让你收获一个漂亮的三重故障（Triple Fault）。

## 架构图

先看 stage2 在内存中的整体布局。MBR 通过 BIOS 扩展读把 stage2.bin 加载到物理地址 `0x8000`， linker script 必须以 `0x8000` 为基址，这样所有绝对地址引用（尤其是 GDT pointer 里的 base 地址）才能在运行时指向正确的物理内存。

```
stage2 内存布局（物理地址）
┌──────────────────────────────────────┐ 0x10000 (32KB 边界, ASSERT 上限)
│                                      │
│         .data section                │
│         (msg_stage2_ok 等)           │
│                                      │
├──────────────────────────────────────┤
│         .rodata section              │
│                                      │
├──────────────────────────────────────┤
│         .gdt section (ALIGN 8)       │
│  ┌──────────────────────────────┐    │
│  │ gdt_ptr (6 bytes)            │    │  ← GDTR 加载源
│  │   limit: 2 bytes             │    │
│  │   base:  4 bytes             │    │
│  ├──────────────────────────────┤    │
│  │ gdt_data (8 bytes)           │    │  ← Selector 0x10
│  │ gdt_code (8 bytes)           │    │  ← Selector 0x08
│  │ gdt_null (8 bytes)           │    │  ← Selector 0x00
│  └──────────────────────────────┘    │
│                                      │
├──────────────────────────────────────┤ 0x8000
│         .text section                │
│         (stage2.S + serial.S)        │
│                                      │
└──────────────────────────────────────┘ 0x8000 (MBR 加载地址)
```

GDT 本身的内部结构是这样的——每个描述符 8 字节，我们定义了三个段描述符加上一个 6 字节的 `gdt_ptr`：

```
GDT 内存布局（.gdt section, 8 字节对齐）

Offset  内容                      Selector
──────  ────────────────────────  ────────
0x0000  Null Descriptor (8B)      0x00
        00 00 00 00 00 00 00 00

0x0008  Code32 Descriptor (8B)    0x08
        FF FF 00 00 00 9A CF 00
        ├───┘ │  │  │  │  │  └─── Base 31:24 = 0x00
        │     │  │  │  │  └────── Flags: 0xCF (G=1,D=1,Limit19:16=0xF)
        │     │  │  │  └───────── Access: 0x9A (P=1,DPL=00,S=1,Type=0xA)
        │     │  │  └──────────── Base 23:16 = 0x00
        │     │  └─────────────── Base 15:0 = 0x0000
        │     └────────────────── Limit 15:0 = 0xFFFF
        └────────────────────────

0x0010  Data32 Descriptor (8B)    0x10
        FF FF 00 00 00 92 CF 00
        (同上，Access = 0x92)

0x0018  ── gdt_end ──

0x0018  gdt_ptr (6 bytes):
        Limit (2B): 0x0017  (= 0x18 - 1 = 23, 即 3个描述符×8 - 1)
        Base  (4B): GDT 的线性地址 (由 linker 填入)
```

你会发现三个描述符的 Base 全是 `0x00000000`，Limit 全是 `0xFFFFF`（配合 Granularity 位就是 4GB）。这就是所谓的 Flat Memory Model——整个地址空间平铺不分段，保护交给后续的分页机制。我们稍后在设计决策部分会聊为什么这么做。

## 代码精讲

### serial.S: 汇编模式修正

我们先看 `boot/common/serial.S` 的变更。这个文件里全是实模式下用的工具函数——`print_string`、`enable_a20`、VESA 相关的一堆操作。问题出在一行 directive 上。

```asm
 .section .text
-.code16gcc                     // Generate 16-bit code (works with --32)
+.code16                        // Generate 16-bit code (works with --32)
```

只有一行改动，但影响非常深远。`.code16gcc` 和 `.code16` 都是告诉 GAS（GNU Assembler）"接下来生成 16 位代码"，区别在于对指令宽度的态度。`.code16gcc` 是专门为 GCC 输出设计的——它允许汇编器生成 32 位操作数前缀（`0x66`）的指令，因为 GCC 在生成 16 位代码时可能会插入 32 位操作。而 `.code16` 则严格得多：它要求所有指令都是 16 位编码，如果你写 `push %ax`，它就老老实实生成 16 位 push。

问题来了：在 `.code16gcc` 下，`push %ax` 和 `pop %ax` 这种不带宽度后缀的指令，汇编器可能会按照自己的理解生成 32 位的 `pushl` 而非 16 位的 `pushw`。在实模式下，栈操作宽度的不一致是致命的——你 `push` 了 4 字节出去，`pop` 只收回 2 字节，SP 寄存器就偏了。我们在调试时确实踩到了这个坑：SP 从 `0xFFFE` 跳到了 `0x000A`，栈帧完全错位，后续的 `ret` 跳到了莫名其妙的地址，直接三重故障。

修复方案就是两步走：把 `.code16gcc` 改成 `.code16`，然后把所有裸的 `push`/`pop` 加上显式宽度后缀 `pushw`/`popw`。

```asm
 .global print_string
 .type print_string, @function
 print_string:
-    push %ax
-    push %bx
-    push %si
+    pushw %ax
+    pushw %bx
+    pushw %si
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
-    pop %si
-    pop %bx
-    pop %ax
+    popw %si
+    popw %bx
+    popw %ax
     ret
```

这里的改动非常机械——每个 `push` 变 `pushw`，每个 `pop` 变 `popw`。整个 `serial.S` 里所有函数都做了同样的处理：`enable_a20`、`vesa_get_controller_info`、`vesa_get_mode_info`、`vesa_save_framebuffer_info`，总共二十多处。改动本身没有技术含量，但不改的后果非常严重。我们回头想一下，这个 bug 的可怕之处在于：在简单场景下（比如函数里只 push/pop 一次），32 位和 16 位 push 的结果可能恰好"看起来对"，因为实模式下的 32 位 push 只是把 SP 多减了 2 而已，数据本身还在栈上。但当调用链变深、多个函数嵌套 push/pop 时，栈偏移就累积起来了，最终在某次 `ret` 时跳到错误地址。

### boot/CMakeLists.txt: Linker Script 修正

接下来是 linker script 的修正，这个改动直接决定了 GDT 能不能被正确加载。

```cmake
 # Create Stage2 linker script
+# Note: Stage2 is loaded by MBR to 0x8000 (see mbr.S:22 STAGE2_LOAD_ADDR)
+# The link address MUST match the load address for absolute addressing to work.
 file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
 "
 OUTPUT_FORMAT(\"elf32-i386\")
 ENTRY(_start)
 SECTIONS
 {
-    . = 0x0;
+    . = 0x8000;
     .text : {
         *(.text)
+    }
+
+    .gdt ALIGN(8) : {
+        *(.gdt)
+    }
+
+    .rodata : {
         *(.rodata)
     }
-    .data : { *(.data) }
-    .bss  : { *(.bss) }
+
+    .data : {
+        *(.data)
+    }
     /DISCARD/ : { *(.comment*) *(.note*) }
 }
 ")
```

两个关键改动。

第一个是 `. = 0x0` 改成 `. = 0x8000`。Linker script 里的这个 `.` 叫 location counter，它决定了链接器为所有符号分配的地址起点。我们之前把它设成 `0x0`，意味着 `gdt_ptr` 里的 `gdt` 符号会被链接成类似 `0x0160` 这样的地址——看起来是个小数字，但 stage2 实际被 MBR 加载到了 `0x8000`，运行时 `gdt` 的真实物理地址应该是 `0x8160`。于是 `lgdt gdt_ptr` 读到的 GDT base 是 `0x0160`，CPU 去这个地址读段描述符——读到的是一片空内存或者完全无关的数据，保护模式切换直接炸。把 base 改成 `0x8000` 之后，链接器会把 `gdt` 符号的地址算成 `0x8160`，与实际加载位置吻合，问题解决。

你可能会问：为什么 stage2 被加载到 `0x8000` 而不是别的地址？这是在 MBR 的 `mbr.S` 里决定的——MBR 通过 BIOS INT 0x13 扩展读把 stage2 加载到 `0x0000:0x8000`，这个地址选择没有特殊含义，只要不跟 MBR 自身（`0x7C00`）、BIOS 数据区（`0x400`-`0x4FF`）、VESA 缓冲区（`0x6000`-`0x64FF`）冲突就行。但关键是：linker script 的 base 必须与这个加载地址严格一致，否则所有绝对地址引用全错。

第二个改动是新增了 `.gdt ALIGN(8)` section。在之前的 linker script 里，`.gdt` section 并不存在——所有代码和数据都堆在 `.text` 和 `.data` 里。如果 GDT 定义放在 `.text` 中间，会有两个问题：一是 GDT 可能不被 8 字节对齐（Intel SDM Vol.3A §3.5.1 建议描述符表 8 字节对齐以提高性能），二是 GDT 数据会被夹杂在可执行代码中间，影响 `.text` 的 cache 局部性。单独拎出来放到 `.gdt` section，用 `ALIGN(8)` 保证对齐，干净又安全。

整理一下改动后的完整 linker script 结构：`.text` 在最前面（从 `0x8000` 开始），`.gdt` 紧随其后并 8 字节对齐，然后是 `.rodata` 和 `.data`。这保证了 GDT 在内存中的位置是确定的、对齐的，且不跟代码混在一起。

### cmake/qemu.cmake: Debug Console

进入保护模式之后，BIOS INT 10h 就不能用了——保护模式下没有 BIOS 中断服务。我们需要一个新的调试输出手段。QEMU 提供了一个非常方便的 debug console（调试控制台），对应端口 `0xE9`：向这个端口写一个字节，QEMU 就会把字符输出到宿主机的终端或文件。不需要初始化任何硬件，不需要串口配置，一行 `outb` 搞定。

```cmake
+# Set the debug console as 0xe9
 set(QEMU_DEBUG_FLAGS
     -s
     -S
+    -debugcon file:debug.log
+    -global isa-debugcon.iobase=0xe9
 )
```

新增了两行 QEMU 启动参数。`-debugcon file:debug.log` 告诉 QEMU 把 debug console 的输出写到 `debug.log` 文件而不是 `stdio`——我们选择写文件是因为 `stdio` 已经被串口（`-serial stdio`）占用了，混在一起不好分辨。`-global isa-debugcon.iobase=0xe9` 把 debug console 的 I/O 端口设为 `0xE9`，这是 OSDev 社区的事实标准约定（Bochs 仿真器最先使用 `0xE9` 作为 debug port，QEMU 兼容了这个约定）。

这个配置加在 `QEMU_DEBUG_FLAGS` 而不是 `QEMU_COMMON_FLAGS` 里，意味着它只在 `make run-debug` 时生效，普通 `make run` 不会产生 `debug.log` 文件。当然，在当前的代码里这两个 flag 已经被移到了 `QEMU_COMMON_FLAGS` 中让所有运行模式都能看到 debug 输出，但 diff 中的原始改动是加在 debug flag 组里的。

使用方式极其简单。在保护模式下的代码里：

```asm
    movb $0x50, %al               // 'P' 的 ASCII 码
    outb %al, $0xE9               // 写到 debug port
```

执行后，`debug.log` 里就会出现字符 `P`。这是我们验证保护模式切换成功的标志——如果看到了 `P`，说明 CPU 成功执行了 32 位代码，段寄存器重载正确，`outb` 指令在保护模式下工作正常。

### stage2.S: GDT 定义

现在来看本篇最核心的部分——GDT 的定义。这段代码在 `stage2.S` 的末尾，位于独立的 `.gdt` section 中。

```asm
// ============================================================
// GDT (Global Descriptor Table) - in .gdt section
// ============================================================
.section .gdt,"a"
.align 8                          // Align to 8 bytes (2^3)

gdt:
gdt_null:
    .quad 0                        // Null descriptor (required)

gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Present, DPL=0, Code, Executable, Readable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_data:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x92                     // Present, DPL=0, Data, Writable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_end:

gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1 = 23 (3 descriptors * 8 - 1)
    .long gdt                      // Linear address of GDT base
```

我们逐段拆解。

首先是 `.section .gdt,"a"` —— 把 GDT 放到独立的 `.gdt` section，`"a"` 标记表示 allocatable（可分配），这是段属性的标准写法。`.align 8` 保证 GDT 起始地址是 8 字节对齐的。Intel SDM Vol.3A §3.5.1 并没有严格要求 GDT 必须 8 字节对齐（只需要字节对齐就行），但对齐访问性能更好，而且描述符本身就是 8 字节的，对齐后每个描述符的起始地址都是整齐的。

**Null 描述符**（`gdt_null`）—— 8 字节全零。Intel 规定 GDT 的第一个条目（索引 0）必须是 null descriptor，而且 CPU 硬件会强制检查：任何试图用 selector `0x00` 访问内存的操作都会触发 General Protection Fault（#GP）。这个设计的目的是让"忘记初始化段寄存器"这种编程错误能被立即捕获，而不是静默地读到随机数据。

**Code32 描述符**（`gdt_code`）—— 定义了一个 32 位代码段。8 字节从低到高排列，Intel 的段描述符格式把 Base 和 Limit 字段拆散在不同的位置：

```
字节布局（从低地址到高地址）：
Byte 0-1:  Limit 15:0    = 0xFFFF
Byte 2-3:  Base 15:0     = 0x0000
Byte 4:    Base 23:16    = 0x00
Byte 5:    Access Byte   = 0x9A
Byte 6:    Flags + Limit 19:16 = 0xCF
Byte 7:    Base 31:24    = 0x00
```

Access Byte `0x9A` 的逐位分解：

```
0x9A = 1001 1010 b

Bit 7   (P=1)    : Present，段在内存中存在
Bit 6-5 (DPL=00) : Descriptor Privilege Level = 0（最高特权级 Ring 0）
Bit 4   (S=1)    : Segment descriptor（不是 system descriptor）
Bit 3   (E=1)    : Executable，这是一个代码段
Bit 2   (DC=0)   : Direction/Conforming，向上生长/非一致代码段
Bit 1   (RW=1)   : Readable，代码段可读（允许读取段内数据）
Bit 0   (A=0)    : Accessed，CPU 自动设置，我们初始化为 0
```

Flags Byte `0xCF` 的逐位分解：

```
0xCF = 1100 1111 b

Bit 7   (G=1)    : Granularity = 4KB（Limit 的单位是 4KB 页而非字节）
Bit 6   (D/B=1)  : Default operation size = 32 位（影响 push/pop/call 的默认宽度）
Bit 5   (L=0)    : Not 64-bit code（64 位模式用另一个描述符）
Bit 4   (0)      : Reserved，必须为 0
Bit 3-0 (0xF)    : Limit 19:16 = 0xF

Limit 计算: 0xFFFF (low) + 0xF << 16 (high) = 0xFFFFF
  配合 G=1 (4KB granularity): 0xFFFFF * 4096 = 4GB - 1
```

所以这个描述符定义的是：基地址 `0x00000000`，限长覆盖整个 4GB 地址空间，32 位代码段，Ring 0 特权级，可读可执行。

**Data32 描述符**（`gdt_data`）—— 和 Code32 几乎一样，唯一区别是 Access Byte 从 `0x9A` 变成了 `0x92`：

```
0x92 = 1001 0010 b

Bit 7   (P=1)    : Present
Bit 6-5 (DPL=00) : Ring 0
Bit 4   (S=1)    : Segment descriptor
Bit 3   (E=0)    : Not executable，这是一个数据段
Bit 2   (DC=0)   : Direction，向上生长
Bit 1   (RW=1)   : Writable，数据段可写
Bit 0   (A=0)    : Accessed
```

数据段不需要可执行权限，但需要可写权限——栈和数据都在这个段里。Flags 是一样的 `0xCF`：4KB 粒度，32 位，Limit 覆盖 4GB。

**gdt_ptr**（GDTR 加载结构）—— 这是 `lgdt` 指令的参数，格式是 6 字节：前 2 字节是 GDT 的 limit（总大小减 1），后 4 字节是 GDT 的线性基地址。`gdt_end - gdt - 1` 由汇编器在编译时算出——三个描述符各 8 字节共 24 字节，减 1 得到 `0x17`（23）。`.long gdt` 是一个 32 位绝对地址引用，链接器会根据 linker script 的 base address `0x8000` 加上 `.gdt` section 的偏移量算出正确的物理地址。

这里有一个很关键的点需要注意：`lgdt` 指令在实模式下执行时，CPU 计算物理地址的方式是 `DS * 16 + offset`。所以我们在 `stage2.S` 中调用 `lgdt gdt_ptr` 之前，必须先确保 `%ds` 为 0：

```asm
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)
    lgdt gdt_ptr                  // Load GDTR from absolute address
```

如果 `%ds` 不为 0（比如还是之前 VESA 操作时设的某个段值），那 `lgdt` 实际读到的 `gdt_ptr` 地址就会偏移 `DS * 16`，读到的 GDT base 地址就是错的，后续进入保护模式时 CPU 加载的段描述符全是垃圾数据——又一个经典的三重故障来源。

## 设计决策

### Flat Memory Model vs 分段保护

你可能会觉得奇怪：既然费了这么大劲定义 GDT，为什么 Code 和 Data 的 Base 都是 `0x00000000`，Limit 都是 4GB？这不等于"没分段"吗？

没错，这就是 Flat Memory Model（平坦内存模型）。在 x86 的历史中，分段保护（Segmentation）是最早的内存保护机制——不同段有不同的 Base 和 Limit，程序不能越界访问。但在现代操作系统里，分段保护基本被废弃了，取而代之的是分页机制（Paging），它提供了更灵活的 4KB 粒度的内存保护。Linux、Windows、macOS 以及教学 OS 如 xv6 和 SerenityOS 都使用 Flat Memory Model：GDT 里 Code 和 Data 都覆盖整个 4GB 空间，Base 为 0，不做任何分段隔离。真正的内存隔离交给后续的页表实现。

我们选择 Flat Model 的原因很简单：第一，这是行业标准的做法，参考 xv6 的 `bootasm.S` 就会发现它的 GDT 和我们几乎一模一样；第二，在 Bootloader 阶段做分段保护没有意义——此时系统里只有一个"程序"在跑，就是 Bootloader 本身，没有隔离的需求；第三，后续实现分页时会重新定义内存布局，GDT 只是保护模式的一个"入场券"，不需要承担太多职责。

### .code16 vs .code16gcc

前文已经讲了这两者的区别，这里从设计决策的角度再总结一下。我们选择 `.code16` 而非 `.code16gcc` 的核心理由是**确定性**：`.code16` 要求所有指令宽度都是显式的，汇编器不会自作主张地加 32 位前缀。这在手写汇编中是正确的工程实践——我们的 Bootloader 是纯汇编写的，每条指令的宽度应该由程序员明确控制，而不是让汇编器猜。

`.code16gcc` 的存在是为了给 GCC 的 C 编译器输出用的。GCC 在 `-m16` 模式下生成的汇编可能混合 16 位和 32 位操作，`.code16gcc` 告诉汇编器"我知道这里可能有 32 位指令，别报错"。但我们是手写汇编，完全没必要接受这种不确定性。改成 `.code16` 之后，如果你写了不带宽度后缀的 `push %ax`，汇编器要么报错要么按 16 位处理——总之行为是明确的，不会偷偷给你塞一个 32 位 push。

## 扩展方向

**1. 多特权级 GDT（Ring 3 用户态）**（中等难度）—— 当前的 GDT 只有 Ring 0 的描述符。要支持用户态程序，需要额外定义 DPL=3 的 Code 和 Data 描述符（Access Byte 中 DPL 字段改为 `11`），以及一个 TSS（Task State Segment）描述符用于 Ring 3 到 Ring 0 的栈切换。这是进入用户态之前必须完成的准备工作。

**2. LDT（局部描述符表）**（较高难度）—— x86 除了 GDT 还支持 LDT，每个进程可以有自己的 LDT 来定义私有的段。现代 OS 基本不用 LDT（全都靠分页），但如果你想深入理解 x86 分段机制的全貌，实现 LDT 是一个很好的练习。Intel SDM Vol.3A §3.5.3 有详细描述。

**3. GDT 自映射（Self-Mapping GDT）**（进阶）—— 在保护模式下修改 GDT 条目需要知道 GDT 的线性地址。一种技巧是在 GDT 中定义一个别名描述符指向 GDT 自身，这样就可以通过段选择子直接读写 GDT 的内容，而不需要额外维护 GDT 的地址。Linux 内核在早期版本中使用过这种技巧。

## 参考资料

- Intel SDM Vol.3A §2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令加载 6 字节（16 位 limit + 32 位 base）到 GDTR
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：8 字节描述符格式，Base/Limit/Access/Flags 字段的分散排列方式
- Intel SDM Vol.3A §10.9.1 — Switching to Protected Mode：完整的 6 步切换流程
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- OSDev Wiki: [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial)
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)
