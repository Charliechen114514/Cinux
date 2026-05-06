# 002-3 · 设计对比与调试实战

## 概览

前两篇我们已经把 GDT 定义、linker script 修正、debugcon 配置、保护模式切换的完整代码都走了一遍。这篇我们换一个角度——退后一步，看看其他项目是怎么处理同一件事的，以及如果事情搞砸了该怎么排查。

这不是简单的"看人家怎么做的"式罗列，而是想通过对比来理解不同工程约束下的不同选择，同时整理出一套系统化的调试方法论。

保护模式切换的调试是 x86 内核开发中最让人头疼的环节之一，因为大部分错误的症状都是同一个：三重故障，CPU 复位，你什么都看不到。本篇最后会给出一个完整的常见 triple fault 原因排查表，按出现频率排序，方便你在出问题时快速定位。

---

## 设计对比

### xv6：教科书级的极简主义

xv6 的 `bootasm.S`（https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S）是 MIT 6.828 课程的教学操作系统，它的 bootloader 把整个 real-to-protected mode 切换塞进了单个 512 字节的引导扇区里。GDT 定义和 Cinux 几乎一模一样——null + code(flat) + data(flat)——只不过它用了宏 `SEG_ASM` 来生成描述符，展开后和我们手写的完全等价。

```asm
# xv6 bootasm.S 中的 GDT 定义（使用宏）
.p2align 2
gdt:
  SEG_NULLASM                             # null seg
  SEG_ASM(STA_X|STA_R, 0, 0xffffffff)    # code seg
  SEG_ASM(STA_W, 0, 0xffffffff)           # data seg
```

`SEG_ASM` 宏展开后生成的描述符值与 Cinux 手写的完全一致——Base=0, Limit=4GB, Code Access=0x9A, Data Access=0x92, Flags=0xCF。不同的表达方式，相同的结果。

值得注意的是，xv6 用 `.p2align 2` 来对齐 GDT（4 字节对齐），而 Cinux 用 `.align 8`（8 字节对齐）。两者都能工作，但 8 字节对齐更符合 Intel 的建议。xv6 选择 4 字节对齐是为了在 512 字节的空间限制下节省空间——每一字节都要精打细算。Cinux 没有 512 字节的限制，所以可以选择更优的对齐方式。

但 xv6 和 Cinux 的工程架构差异很大。xv6 没有独立的 stage2——它把模式切换、A20 开启、内核加载全部放在 512 字节里。这意味着 xv6 不能做 VESA 图形初始化（没有空间），也不能做复杂的错误处理（没有空间打印调试信息）。

A20 开启方式也不同：xv6 用键盘控制器 8042（端口 0x64/0x60，所谓的 "fast A20" 方式），而 Cinux 用 BIOS INT 0x15 AX=0x2401。两种方式各有优劣——8042 方式更快（不需要 BIOS 调用的开销），但兼容性稍差（某些虚拟机和旧硬件上可能不支持）；BIOS 方式更可靠，但需要依赖 BIOS 实现。

xv6 的栈设置也不同于 Cinux：xv6 在保护模式切换后把栈指针设为 `0x7C00`（boot sector 自身的位置，向下增长），而 Cinux 使用 `0x90000`。xv6 的选择是合理的——在单扇区架构下，boot sector 之上的内存都可以用作栈空间，没有必要单独选一个远处的地址。

但 Cinux 的两阶段设计让 stage2 的代码和数据占据 `0x8000` 开始的区域，使用 `0x90000` 作为栈顶可以避免与代码区域冲突。

从 `.code16` 的使用来看，两者是一致的。xv6 从第一行起就是 `.code16`，Cinux 在 tag 002 修正后也改成了 `.code16`。这个选择在所有手写 16 位引导代码中是共识——`.code16gcc` 是给 C 编译器用的，手写汇编就应该用 `.code16`。

### 对比总结

总结一下各项目的关键差异：xv6 是单扇区架构，所有东西塞在 512 字节里，栈设在 `0x7C00`，A20 用 8042 方式；Cinux 是两阶段架构，stage2 独立加载到 `0x8000`，栈设在 `0x90000`，A20 用 BIOS 方式；Linux 用 GRUB 跳过了整个 bootloader 的工作；SerenityOS 也用 GRUB；ToaruOS 直接跳到 long mode 跳过了保护模式。

这些差异没有"哪个更好"的问题——每个选择都是在特定约束下的权衡。xv6 的极简主义适合教学演示，Cinux 的分阶段设计适合逐步学习，Linux 用 GRUB 是工程效率的考虑。

### Linux：用 GRUB 跳过整个问题

Linux 的做法和 xv6、Cinux 都不一样——它根本不在内核里做 real-to-protected mode 切换。Linux 使用 GRUB（或其他符合 Multiboot 规范的 bootloader）来启动，GRUB 在把内核加载到内存之前就已经完成了 real mode -> protected mode -> 32-bit flat mode 的全部切换。内核被加载时，CPU 已经运行在 32 位保护模式下，内核的第一条指令就是 32 位代码。

这个选择背后的权衡很明确：Linux 内核的开发者认为 bootloader 的工作不应该由内核来承担。GRUB 是一个成熟的外部项目，它处理了各种硬件初始化的脏活累活——A20 开启、VESA 模式设置、保护模式切换、甚至可以从文件系统读取内核映像。

Linux 内核的 `arch/x86/boot/` 目录下确实有 real mode 的 setup 代码，但那是为了支持 Linux 自己的 boot protocol（https://www.kernel.org/doc/html/latest/arch/x86/boot.html ），让非 GRUB 的 bootloader 也能启动 Linux。

对于 Cinux 来说，选择自己写 bootloader 而不是用 GRUB，是一个有意识的权衡。教学目的下的自写 bootloader 有两个好处：第一，你能看到完整的启动链条（从 BIOS POST 到实模式代码执行到保护模式切换），每一步都是显式的、可调试的；第二，你能精确控制内存布局，比如 Cinux 的 VESA 初始化就是在 GRUB 不会帮你做的范围之内。代价当然是更多的代码量和更多的调试时间——但如果你是在学习操作系统，这些恰恰是你想经历的。

### SerenityOS 和 ToaruOS：不同的路径

SerenityOS 使用 GRUB，和 Linux 一样跳过了 bootloader 的编写。ToaruOS 有自写 bootloader，但它直接从实模式跳到 long mode（64 位），跳过了 32 位保护模式这个中间阶段——这在现代系统上是完全可行的，因为 x86 的架构允许从实模式直接切换到 long mode（通过设置 CR0.PE 和 EFER.LME）。

Cinux 的分阶段设计（MBR -> Stage2 real mode -> Stage2 protected mode -> long mode）在教学上更有价值，因为它让读者能看到每一步的完整过程。如果直接跳到 long mode，你就错过了理解 GDT、段描述符、保护模式地址翻译这些基础知识的机会——而这些知识在调试 x86 相关的问题时是不可或缺的。

### Flat Memory Model：所有人的共识

上面这些项目虽然架构差异很大，但在 GDT 的设计上有一个共同的选择：Flat Memory Model。所有项目的代码段和数据段都是 Base=0、Limit=4GB，不做任何分段隔离。这不是巧合，而是现代操作系统设计的共识。

在 x86 的历史中，分段保护（Segmentation）是最早的内存保护机制——不同段有不同的 Base 和 Limit，程序不能越界访问。但在现代操作系统里，分段保护基本被废弃了，取而代之的是分页机制（Paging），它提供了更灵活的 4KB 粒度的内存保护。Linux、Windows、macOS 以及所有教学 OS 都使用 Flat Memory Model：GDT 里代码和数据都覆盖整个地址空间，Base 为 0，不做任何分段隔离，真正的内存隔离交给后续的页表实现。

---

## 调试方法论

### GDB 断点调试

在保护模式切换这种底层代码里，GDB 是你最可靠的朋友。启动 QEMU 的调试模式：

```bash
cd build
make run-debug
```

然后在另一个终端连上 GDB：

```bash
gdb build/boot/stage2
(gdb) target remote :1234
```

注意这里要用 ELF 文件（`build/boot/stage2`）而不是 bin 文件（`build/boot/stage2.bin`）。ELF 文件包含符号表和调试信息，GDB 才能正确识别函数名和行号；bin 文件只是裸二进制，没有任何元数据，GDB 拿到它就像看一堆字节，断点和符号全都对不上。

在 GDB 中可以设置几个关键断点：在 `_start` 处确认 Stage2 入口正常，在远跳转指令处确认 lgdt 已经执行完毕，在 `pm_entry` 处确认保护模式入口被正确跳转到。到达 `pm_entry` 后，用 `info registers` 查看 CR0 的 PE 位是否为 1，段寄存器是否指向正确的选择子。

### QEMU `-d int` 查异常

如果你在保护模式切换中遇到了三重故障，QEMU 的 `-d int` 选项是定位问题的利器：

```bash
qemu-system-x86_64 -d int -no-reboot -D qemu_int.log -serial stdio -debugcon stdio -drive format=raw,file=build/cinux.img
```

`-d int` 会让 QEMU 在每次中断/异常发生时打印详细信息，包括异常类型、错误码、触发时的 CS:EIP。`-no-reboot` 防止 CPU 在三重故障后自动重启，这样你可以看到完整的异常链。典型的输出可能像这样：

```
check_exception old: 0xffffffff new 0xd
     0: v=0d ecode=0x00000000 eip=0x0000816c cs=0x0008
```

这告诉你 CPU 在地址 `0x816c` 处触发了一个 General Protection Fault（异常号 0x0D），CS=0x0008 说明此时已经在保护模式下（使用了选择子 0x08）。如果异常发生在 `ljmp` 之前，CS 可能还是实模式的值，说明 GDT 内容有问题。

### debugcon 面包屑调试

我们在代码中使用的 `outb 'P', 0xE9` 是最简单的验证手段。但你可以在切换流程的每一步后面都插一个不同的字符输出，形成一条"面包屑"式的调试链：

```asm
    movb $'1', %al; outb %al, $0xE9    // 1: cli done
    movw $0, %ds
    movb $'2', %al; outb %al, $0xE9    // 2: DS=0 done
    lgdt gdt_ptr
    movb $'3', %al; outb %al, $0xE9    // 3: lgdt done
    // ...
```

如果 `debug.log` 里有 `123` 但没有 `4`，你就知道问题出在 CR0.PE 和 far jump 之间。这种逐步插入调试点的方法虽然朴素，但在 Bootloader 这种"没有任何输出手段"的环境里，它往往是最有效的。

---

## 常见 Triple Fault 排查表

下面是我们实际踩过的坑和对应的排查方向，按出现频率排序。

**lgdt 读到了错误的 GDT 地址。** 这是最常见的坑，原因是 `lgdt` 前 DS 没有清零。实模式下 `lgdt` 用 `DS × 16 + offset` 计算地址，DS 不为 0 就会读到错误位置。排查方法：在 `lgdt` 前后各加一个 debugcon 输出，如果前一个字符出现了后一个没有，大概率是 `lgdt` 导致的三重故障。

**GDT 描述符定义错误。** Access Byte 写错一位、Limit 不够大、Base 地址错误，这些都会导致 `ljmp` 时 CPU 查到无效描述符触发 #GP。排查方法：用 `objdump -s -j .gdt build/boot/stage2` 检查编译后的 `.gdt` section，逐字节对照 Intel SDM Vol.3A Section 3.4.5 的描述符格式。

**linker script base 地址不匹配。** stage2 被加载到 `0x8000`，但 linker script 的 location counter 设成了 `0x0`，导致所有绝对地址引用偏移 `0x8000`。排查方法：用 `readelf -s build/boot/stage2 | grep gdt` 查看 `gdt` 符号的链接地址，如果比预期小 `0x8000`，就是 base 没设对。

**省略了 far jump。** CR0.PE 设完之后直接写 32 位代码，没有 `ljmp` 刷新 CS 的隐藏缓存。CPU 还在用 16 位模式解码，32 位编码的指令被解析成乱码。排查方法：检查 `movl %eax, %cr0` 后面是否紧跟 `ljmp`，中间不能有任何其他指令。

**`.code16` 和 `.code32` 放错了位置。** `.code32` 放在了 `ljmp` 之前（汇编器提前生成 32 位编码但 CPU 还在 16 位模式），或者 `.code32` 放在了 `ljmp` 之后但离 `pm_entry` 标签太远（导致 `pm_entry` 处的前几条指令仍然是 16 位编码）。排查方法：用 `objdump -d build/boot/stage2` 检查反汇编，确认 `pm_entry` 后的指令是 32 位编码。

**push/pop 宽度不一致。** 如果在 `serial.S` 里还有遗漏的裸 push/pop（没有加 `w` 后缀），在 `.code16` 模式下虽然默认是 16 位，但某些边缘情况下汇编器的行为可能不符合预期。排查方法：用 `grep -n 'push\s\|pop\s' boot/common/serial.S` 搜索所有不带宽度后缀的 push/pop，确保每个都加了 `w`。

---

## 排查流程图

当你遇到 triple fault 时，可以按照以下顺序逐步排查。首先检查 QEMU 是否反复重启——如果是，加 `-d int -no-reboot` 看 CPU 在哪个地址触发了什么异常。如果看到 General Protection Fault 且 CS=0x0008，说明已经进入保护模式但段描述符有问题——检查 GDT 的 Access Byte 和 Flags 是否正确。如果 CS 还是实模式的值，说明问题出在 `ljmp` 之前——检查 `lgdt` 是否读到了正确的地址（DS 是否清零）。如果连 `lgdt` 都没执行到，检查之前的 `cli` 是否正常——可能是 `sti` 后的某个中断触发了 triple fault。

如果以上都没问题，检查 linker script 的 base 地址是否为 `0x8000`——用 `readelf -s build/boot/stage2 | grep gdt` 看符号地址是否在 `0x8000` 以上。如果地址偏低，就是 base 没设对。

最后，如果所有检查都通过了但仍然 triple fault，尝试用 debugcon 面包屑法——在每一步后面插一个不同的字符输出，通过 `debug.log` 里哪些字符出现了、哪些没出现来精确定位崩溃发生在哪一步。

---

## 参考资料

- Intel SDM Vol.3A Section 10.9.1 — Switching to Protected Mode：完整的 6 步切换流程
- Intel SDM Vol.3A Section 3.4.5 — Segment Descriptors：8 字节描述符格式，用于排查 GDT 描述符定义错误
- Intel SDM Vol.3A Section 2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令的行为和 GDTR 寄存器格式
- OSDev Wiki: [Protected Mode](https://wiki.osdev.org/Protected_Mode) — 进入保护模式的步骤和常见陷阱
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)
- Linux Boot Protocol: [https://www.kernel.org/doc/html/latest/arch/x86/boot.html](https://www.kernel.org/doc/html/latest/arch/x86/boot.html)
