---
title: 004-boot-load-mini-kernel-b-3 · 内核加载 (B)
---

# Bootloader 开发日记 004B（下）：保护模式的优雅"无操作"与构建系统改造

> 标签：x86, Protected Mode, flat binary, objcopy, linker script, build system
> 前置：[004B（中）分块磁盘读取与栈冲突](004-boot-load-mini-kernel-b-2.md)

## 写在前面

前两篇我们定义了 BootInfo 结构体，完成了 832 扇区的分块磁盘读取，解决了栈冲突问题。现在进入 004B 的收尾工作：为什么 Protected Mode 什么都不做、构建系统如何配合 flat binary 方案、以及本章的整体设计决策回顾。

## Protected Mode：优雅的"无操作"

我们回头看 Stage2 里 `load_kernel_from_disk` 的调用位置：

```asm
    call query_memory_map           // [->0x5000] E820 memory map
    call load_kernel_from_disk      // [->0x20000] Load mini kernel (416KB, 832 sectors)
    cli                             // disable interrupts
```

调用位置和 004A 完全相同——在 VESA 之后、保护模式切换之前。`cli` 紧跟其后，因为接下来的 GDT 加载和模式切换是临界区。832 个扇区（416KB）的读取全部在这里完成，一口气读完。

然后是 Protected Mode 入口：

```asm
.code32
pm_entry:
    movw $0x10, %ax               // Data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x90000, %esp           // New stack in protected mode

    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Nothing to do in protected mode, proceed to long mode.

    call setup_page_tables
    call enter_long_mode
```

你会发现 Protected Mode 入口和 004A 相比只多了一块注释——"No operation needed"。段寄存器设置、栈初始化、debugcon 输出、页表建立、Long Mode 切换——这些全都是之前就有的代码。004B 没有在 Protected Mode 里新增任何一条指令。

这个"无操作"设计背后的原因是多方面的。首先是架构纯粹性——Intel SDM Vol.3A Section 9.1 定义的处理器状态转换流程是 Real Mode -> Protected Mode -> Long Mode，每一层都有明确的职责。Real Mode 负责 BIOS 调用和硬件初始化，Protected Mode 负责设置 32 位环境（段寄存器、栈），Long Mode 负责建立 64 位执行环境。如果我们在 Protected Mode 里混入磁盘读取或数据搬运的操作，就打破了这种清晰的分层。

其次是内存布局一致性。小内核已经通过 flat binary 方式加载到了 0x20000——它在磁盘镜像上的位置和在内存里的位置完全对应，不需要任何重定位或搬移。如果 Protected Mode 里要做 ELF 解析，那确实需要在这个阶段做额外工作（读 Program Header、搬移段、清零 BSS）。但我们选了 flat binary，这些工作全部省掉了。

最后是 BIOS 中断的限制。保护模式下 IVT 被 IDT 替换，INT 13h 不再可用。如果要在保护模式下读磁盘，就得像 xv6 那样自己实现 IDE PIO 驱动。对于一个教学 bootloader 来说，在 Real Mode 下用 BIOS 一次性读完所有数据，然后在保护模式和 Long Mode 里不做任何磁盘操作，是最简单的方案。

## 构建系统变更

004B 对构建系统的改动主要集中在三个方面：链接脚本地址修改、objcopy 后处理步骤、build_image.sh 的大小限制检查。

### 链接脚本：物理地址从 0x200000 到 0x20000

小内核的链接脚本 `kernel/mini/linker.ld` 把物理起始地址从 004A 的 0x200000（2MB）改到了 0x20000（128KB）：

```
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start must be first! */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
}
```

物理起始地址 0x20000 是本章所有改动的锚点。链接地址和加载地址必须完全一致——如果链接脚本写 0x20000 但 bootloader 把数据加载到了 0x30000，所有绝对地址引用（函数调用、全局变量访问）全部指向错误的内存位置。`.text.start` 段排在最前面，确保 `_start` 是输出文件的第一个符号——flat binary 的第一个字节就是入口点，没有 ELF header 帮你找 entry point。

VMA（虚拟地址）设为 `0xFFFFFFFF80020000`（高半核），LMA（加载地址）设为 `0x20000`。`AT(ADDR(.text) - KERNEL_Virt_BASE)` 这个表达式告诉链接器：虽然代码的虚拟地址在 0xFFFFFFFF80000000 以上，但它应该被加载到物理地址 0x20000。Long Mode 的页表会同时映射 identity mapping（0x20000 -> 0x20000）和 higher-half mapping（0xFFFFFFFF80020000 -> 0x20000），所以 CPU 用哪个地址都能访问到同一块物理内存。

`.init_array` 段保留了 C++ 全局构造函数的指针数组。BSS 段导出了 `__bss_start` 和 `__bss_end`，供内核入口汇编代码使用。

这里要特别强调 `.text.start` 段的重要性。flat binary 没有 ELF header 来告诉加载器"入口点在哪里"——它默认从文件的第一个字节开始执行。所以链接脚本里 `*(.text.start)` 必须排在 `.text` 通配符之前，确保 `_start` 的代码是输出文件的第一段内容。如果你把通配符顺序写反了（先 `*(.text)` 再 `*(.text.start)`），其他 .text 子段可能会排在 _start 前面，flat binary 的入口就错了。这个 bug 在编译阶段不会报错，只有在运行时才会触发——你跳到了一段意料之外的代码，大概率 triple fault。

### objcopy 后处理：ELF 到 flat binary

构建流程的最后一步是把 ELF 可执行文件转成 flat binary。这一步在 CMakeLists.txt 的 POST_BUILD 命令里完成：

```cmake
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel>
        $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary"
    VERBATIM
)
```

`objcopy -O binary` 做的事情非常直接：剥离所有 ELF header、section header table、符号表、重定位信息，只保留 LOAD 段的原始二进制内容。输出文件的第一个字节就是 `.text.start` 段的第一个字节——`_start` 的第一条指令 `cli`（0xFA）。这个文件会被 build_image.sh 写到磁盘镜像的 LBA 16 位置，然后 boot loader 的 `load_kernel_from_disk` 把它原封不动地读到物理 0x20000。

值得注意的是，链接器本身仍然需要 ELF 格式来工作——它需要解析符号、处理重定位、计算地址。flat binary 是最终产物，不是中间产物。所以编译流程是：源代码 -> ELF 可执行文件（链接器输出）-> flat binary（objcopy 转换）-> 磁盘镜像（dd 写入）。

### build_image.sh：大小限制检查

`build_image.sh` 的关键变更是使用 flat binary 替代 ELF 文件，并加上了严格的大小限制检查：

```bash
MINI_BIN=${3:-${BUILD_DIR}/kernel/mini/mini_kernel.bin}

MINI_KERNEL_MAX_BYTES=$((416 * 1024))  # 425984 bytes
MINI_KERNEL_MAX_SECTORS=$((MINI_KERNEL_MAX_BYTES / 512))  # 832 sectors

if [ $MINI_SIZE -gt $MINI_KERNEL_MAX_BYTES ]; then
    log_error "Mini kernel too large!"
    log_error "  Actual:   $MINI_SIZE bytes ($MINI_SECTORS sectors)"
    log_error "  Maximum:  $MINI_KERNEL_MAX_BYTES bytes (416KB, 832 sectors)"
    # ... detailed error context ...
    exit 1
fi
```

416KB 这个上限来自内存布局的硬约束。加载区域是 0x20000~0x88000（0x20000 + 416KB），再往上就是 Protected Mode 栈（0x90000）减去 32KB 安全间隙的上限。如果小内核超过这个大小，构建直接失败并打印详细的错误信息和内存布局约束说明——这种 fail-fast 的设计比运行时才发现数据写到了栈区要好得多。

磁盘镜像的组装依次写入 MBR（扇区 0）、Stage2（扇区 1~15）、小内核 flat binary（扇区 16 开始）。`conv=notrunc` 确保 dd 不会截断输出文件。

这里有一个调试技巧：如果你怀疑 flat binary 写入磁盘的位置不对，可以用 `xxd` 直接检查磁盘镜像。LBA 16 对应的偏移是 0x2000（16 * 512 = 8192），执行 `xxd build/cinux.img | grep "00002000:"` 就能看到那个位置的数据。如果看到 `7f45 4c46`（ELF magic），说明写入的是 ELF 文件而不是 flat binary——检查 build_image.sh 的第三个参数是否指向 mini_kernel.bin。正确的 flat binary 开头应该是 `fa`（cli 指令的机器码），后面跟着 CR4 操作的编码。

## 设计决策回顾

### 决策 1：Real Mode 完成全部磁盘读取

我们在 Real Mode 的最后阶段（进入 Protected Mode 之前）完成全部磁盘读取。一旦读取完成，内核镜像就安全地躺在 0x20000，后续的模式切换不会再动它。理论上可以在 Protected Mode 或 Long Mode 读取磁盘，但那需要自己写磁盘驱动（IDE PIO 或 AHCI），复杂度远超 BIOS INT 13h 调用。Real Mode 是 BIOS 调用的"黄金窗口"，放弃这个机会纯属自找麻烦。

### 决策 2：固定地址 0x20000

小内核的加载地址硬编码为 0x20000，在链接脚本、boot.S 常量、build_image.sh 中保持一致。更灵活的设计是通过配置文件或链接脚本约定加载地址，但对于当前阶段，固定地址是最简单可靠的。0x20000 这个地址来自对低 1MB 内存布局的仔细分析：避开 Real Mode 栈（0x9000~0x19000），留足安全间隙（28KB），同时不超过 Protected Mode 栈（0x90000）减去 32KB gap 的上限。

### 决策 3：Protected Mode 不做内核相关操作

所有数据搬运（E820 探测 + 832 扇区磁盘读取）在 Real Mode 下全部完成。Protected Mode 只负责 CPU 状态转换——设置段寄存器、初始化栈、建页表、切 Long Mode。这种职责分离让代码更清晰，也避免了在 Protected Mode 处理 BIOS 兼容性问题。

### 决策 4：BootInfo 定义和填充分离

004B 定义 BootInfo 结构体和字段约定，004C 在 Long Mode 入口处执行实际填充。这个分离的设计让每个 tag 的变更范围更小、更容易 review——如果你只关心结构体定义，看 004B 就够了；如果你关心数据搬运的具体实现，看 004C。

这种分离还带来了一个隐含的好处：如果将来我们想修改 BootInfo 的字段（比如添加 ACPI 表地址、SMP 信息），只需要在 `boot_info.h` 中添加字段、更新 `static_assert` 的大小，然后在 004C 的填充代码中添加对应的赋值。004B 的磁盘读取和地址计算逻辑完全不受影响。

## 收尾

来验证一下本章的成果。构建并运行 QEMU，检查 debugcon 日志文件。如果一切正常，你应该看到三个字符：`OPL`。'O' 是 `load_kernel_from_disk` 在 Real Mode 下成功读完 832 个扇区后输出的，'P' 是 Stage2 进入 Protected Mode 后输出的，'L' 是进入 Long Mode 后输出的。这三个字符构成了一条完整的启动状态链——任何一个字符缺失都意味着对应的阶段出了问题。

如果你想更深入地验证小内核确实被正确加载到了 0x20000，可以在 QEMU 里用 GDB 连接后执行 `x/10i 0x20000`——你应该能看到 boot.S 的前几条指令（`cli`、CR4 操作等），而不是 004A 时代的 ELF magic `0x7F 0x45 0x4C 0x46`。如果你看到的还是 ELF magic，说明 build_image.sh 写入的是 ELF 文件而不是 flat binary——检查 CMakeLists.txt 的 POST_BUILD 步骤是否正确生成了 mini_kernel.bin。另外，你可以用 `file build/kernel/mini/mini_kernel.bin` 命令确认文件格式——如果输出是 "ELF 64-bit LSB executable"，说明 objcopy 步骤没有正确执行或被跳过了；正确的输出应该是 "data"（因为 flat binary 没有文件头）。

回过头看，004B 做了四件事。第一，定义了 BootInfo 结构体作为 bootloader 和内核的正式交接契约。第二，把磁盘读取从 8 扇区扩展到 832 扇区，用循环分块读取绕过 BIOS 的 127 扇区限制。第三，解决了栈冲突 bug——加载地址从 0x10000 提高到 0x20000。第四，把小内核从 ELF 格式切换为 flat binary，简化了加载逻辑。

踩的坑主要是三个：栈冲突（0x10000 覆盖 Real Mode 栈导致 ret 跳飞）、BX/BP 寄存器被 BIOS 破坏（循环计数乱套）、ES 段地址在循环中必须每次重设（BIOS 不保证保留调用者的段寄存器）。每一个都是那种"知道了觉得理所当然、不知道时能调一整天"的坑。

004B 结束后的状态是：bootloader 能在 Real Mode 把完整的 416KB flat binary 从磁盘加载到 0x20000，穿过 Protected Mode，进入 Long Mode 并输出 `OPL`。但此时 bootloader 还没有把控制权交给内核——Long Mode 入口处只是 `hlt` 停住。真正的跳转和 BootInfo 填充留给 004C 完成。从功能完整性的角度看，004B 是一个"准备阶段"——所有数据都就位了，但还没有"交接"。

你可以把这个阶段类比为搬家：004A 是确认新家的地址和路线（E820 探测 + 试读 4KB），004B 是把全部家具搬进新家（416KB 完整加载），004C 是最后入住（填充 BootInfo + 跳转内核）。每一步都有清晰的边界，出了问题也容易定位。

下一篇（tag 004C-1）我们将完成最后的拼图——在 Long Mode 里填充 BootInfo 结构体（把 E820 数据从 0x5000、VESA 数据从 0x6400 搬到 0x7000 的 BootInfo 里），然后跳转到小内核的入口地址，让那个 'M' 字符真正出现在 debugcon 日志里。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。Real Mode / Protected Mode / Long Mode 的状态转换。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=42h 扩展读取规范。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — Rolling Your Own Bootloader: 自写 bootloader 的综合指南。
  https://wiki.osdev.org/Rolling_Your_Own_Bootloader

- Linux x86 Boot Protocol: `struct boot_params` 的设计理念。
  https://www.kernel.org/doc/html/latest/arch/x86/boot.html

- xv6 bootmain.c (MIT): IDE PIO 磁盘读取的参考实现。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
