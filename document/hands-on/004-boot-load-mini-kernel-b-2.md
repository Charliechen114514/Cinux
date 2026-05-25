---
title: 004-boot-load-mini-kernel-b-2 · 内核加载 (B)
---

# 004 加载小内核（磁盘读取升级篇） -- 从 4KB 到 416KB 的跨越

> 本章完成后的可见效果：QEMU debugcon 输出 `O`（磁盘读取成功），后续接着输出 `P` 和 `L`
>
> 前置要求：已完成 [004B-1 BootInfo 结构定义](004-boot-load-mini-kernel-b-1.md)

---

## 导语

上一篇我们定义了 BootInfo 结构体，确立了 bootloader 和内核之间的交接契约。这一篇进入本章最核心的部分：把磁盘读取从 8 扇区（4KB）扩展到 832 扇区（416KB），用循环分块读取绕过 BIOS 的 127 扇区限制。这一步涉及大量的段地址动态计算、寄存器保护、DAP 结构重建——每一个环节都有坑。更刺激的是，在这个过程中我们会遇到一个栈冲突 bug，迫使我们把加载地址从 0x10000 改到 0x20000。

**这一步之后，我们就能：**
- 在 Real Mode 下完成完整小内核（416KB）的磁盘加载
- 理解 BIOS 127 扇区限制和分块循环读取策略
- 避开栈冲突，使用安全的加载地址 0x20000

---

## 一、概念精讲

### 1.1 BIOS 127 扇区限制和分块策略

004A 读了 8 个扇区（4KB），一次 INT 13h 调用就搞定了。现在我们要读 832 个扇区（416KB），一次读不完——OSDev Wiki 的 "Disk access using the BIOS (INT 13h)" 页面明确指出，INT 13h AH=42h 扩展读取的单次上限是 127 个扇区（某些 BIOS 实现甚至更低）。这不是 INT 13h 规范本身的硬限制，而是 BIOS 实现中 DMA 传输缓冲区的约束——BIOS 内部需要把 DMA 传输限制在 64KB 边界内，127 个扇区 = 65024 字节，恰好不超过这个限制。

解决方案是分块循环读取：每次读最多 127 个扇区，读完一批后更新参数读下一批，直到全部读完。具体来说，我们用一个寄存器跟踪"已经读了多少扇区"（BX），每轮循环先计算"还剩多少扇区"（总数减去已读），然后取"剩余扇区"和 127 的较小值作为本轮读取量。DAP 里的 LBA 需要每轮更新（起始 LBA + 已读扇区数），buffer 地址也需要每轮移动——因为每轮读出的数据必须紧接着上一轮的数据存放，这样最终在内存里才能形成连续的内核镜像。buffer 地址的计算公式是：基础段地址 + 已读扇区数乘以 32（因为每个扇区 512 字节，512 / 16 = 32 个"段单位"，而段地址每加 1 对应物理地址加 16 字节）。

这个循环逻辑本身不复杂，但有一个特别阴险的坑：BIOS 调用会破坏 BX 和 BP 寄存器的值。Intel SDM 没有明确列出"哪些寄存器会被 BIOS 破坏"，但实际测试表明，很多 BIOS 实现在 INT 13h 内部会使用 BX 和 BP 作为临时寄存器且不恢复。如果你用 BX 来跟踪已读扇区数却不保存，循环第二轮的 BX 就是一个垃圾值——要么读到错误的地址，要么 LBA 偏移完全错乱，甚至可能死循环。所以每次 BIOS 调用前必须 pushw 保存 BX 和 BP，调用后 popw 恢复。

### 1.2 栈冲突惊魂记：0x10000 为什么不行

上一章我们把内核 ELF header 加载到 0x10000，那时候只读 4KB 没出问题。但如果你把完整内核（最大 416KB）也加载到 0x10000，事情就炸了——因为 Real Mode 的栈就在附近。

Stage2 的栈设置是 SS=0x0900、SP=0xFFFE，对应的物理地址范围是 0x9000 到 0x19000（栈从高地址往低地址增长，物理地址 = SS << 4 + SP = 0x9000 + 0xFFFE = 0x19000，向下增长到 0x9000）。如果你把内核从 0x10000 开始加载 416KB，磁盘写入的数据会从 0x10000 一路写到 0x71000——而 0x10000 到 0x19000 这段区域正好覆盖了栈空间！

崩溃的机制是这样的：磁盘读取把数据写到了 0x10000~0x19000 的区域，覆盖了栈里保存的返回地址。当 `load_kernel_from_disk` 函数执行 `ret` 想返回 stage2 的时候，从栈里弹出的返回地址已经被磁盘数据覆盖了——CPU 跳到一个莫名其妙的地址，大概率是一个非法指令或者一个空指针，然后 triple fault 重启。这种 bug 的恐怖之处在于：它不是"读错了数据"，而是"数据写对了地方但那个地方恰好是栈"——所以你检查 0x10000 处的数据会发现内核确实被正确加载了，但函数就是回不去。

解决方案是把加载地址从 0x10000 改到 0x20000。判断标准是：加载地址必须大于栈顶物理地址（0x19000），0x20000 - 0x19000 = 0x7000 = 28KB 的安全间隙绰绰有余。内核最大 416KB，从 0x20000 加载到 0x88000，而 Protected Mode 的栈在 0x90000，中间还有 32KB 的安全距离。

### 1.3 Protected Mode "无操作"：Real Mode 干完所有活

在上一章的设计里，Protected Mode 本来还需要做一些数据搬运的工作（比如解析 ELF header 后加载各个段）。但既然现在我们切换到了 flat binary，所有数据搬运（E820 探测 + 完整内核加载）都可以在 Real Mode 下完成——因为 Real Mode 有 BIOS 中断可用，可以方便地读磁盘。Protected Mode 阶段就只需要做三件事：设置段寄存器（DS、ES、FS、GS、SS 全部设为 32 位数据段选择子）、初始化栈（ESP = 0x90000）、输出一个 'P' 字符到 debugcon 表示"已进入保护模式"——然后直接进入 Long Mode 设置。Protected Mode 在这里纯粹是一个过渡阶段，不做任何实质性的数据操作。

---

## 二、动手实现

### Step 3: 改造磁盘读取为分块循环

**目标**：修改 `boot/common/boot.S` 中的 `load_kernel_from_disk` 函数，将一次读取 8 个扇区改为循环读取 832 个扇区（416KB），每次最多读 127 个扇区。

**设计思路**：函数的核心是一个循环。你需要三个关键变量：BX 记录"已读扇区数"（初始为 0），BP 保存"本轮要读的扇区数"，以及一个固定的 DAP 结构（每次循环更新里面的字段）。每轮循环的第一件事是计算本轮读取量——用总扇区数（832）减去已读扇区数（BX）得到剩余量，然后和 127 比较，取较小值存入 BP。然后构建 DAP：扇区数填 BP，buffer 段地址动态计算（MINI_KERNEL_LOAD_SEG + BX * 32，因为每读一个扇区段地址要前进 512/16 = 32），LBA 也动态计算（MINI_KERNEL_LBA + BX）。调用 INT 13h 后检查 CF，成功则 BX += BP 继续循环，失败则跳转到错误处理。循环退出条件是 BX >= MINI_KERNEL_SECTORS（832）。

关键的常量需要更新：MINI_KERNEL_SECTORS 从 8 改为 832，MINI_KERNEL_LOAD_PHYS 从 0x10000 改为 0x20000，MINI_KERNEL_LOAD_SEG 从 0x1000 改为 0x2000。新增常量 DISK_MAX_SECTORS_PER_CALL = 127 表示 BIOS 单次读取上限。buffer 段地址的动态计算是这样的：物理地址 = MINI_KERNEL_LOAD_PHYS + BX * 512，段地址 = 物理地址 >> 4 = MINI_KERNEL_LOAD_SEG + BX * (512/16) = MINI_KERNEL_LOAD_SEG + BX * 32。由于 BX 最大值是 832，BX*32 = 26624 = 0x6800，加上 MINI_KERNEL_LOAD_SEG = 0x2000 得到 0x8800，对应物理地址 0x88000——正好在 Protected Mode 栈（0x90000）之前。

寄存器保存方面有一个重要变化：函数开头用 `pusha`（不是 `pushaw`）保存 16 位寄存器。在循环内部，每次 BIOS 调用前还需要额外 pushw 保存 BX 和 BP——因为 BIOS 会破坏它们。调用后先 popw 恢复 BP 和 BX，然后再把 BP 加到 BX 上更新已读扇区数。

成功时输出 'O'（0x4F）到 debugcon 表示磁盘读取完成，失败时输出 'F' 并跳转到 panic 错误处理。

**踩坑预警**：这是整个 tag 里坑最密集的地方。第一个坑：BIOS 会破坏 BX 和 BP。如果你不保存，循环第二轮 BX 就变成了垃圾值，buffer 地址和 LBA 全部乱套，轻则读到错误数据，重则直接卡死。第二个坑：INT 13h AH=42h 要求 DS:SI 指向 DAP——不是 ES:SI，也不是 ES:DI。上一章 E820 用的是 ES:DI，如果你按惯性写成 ES:SI，BIOS 读到的是一个空 DAP，返回一个无效参数错误。第三个坑：DAP 的 8 字节 LBA 字段中，高 32 位必须显式清零。不能假定 DAP 所在的 0x7B00 区域内存是干净的——QEMU 里可能碰巧是零，但真实硬件上几乎必然有残留数据，高位垃圾值会让 BIOS 尝试读取一个天文数字般的 LBA，直接报错。第四个坑：在 Real Mode 下应该用 `pusha`/`popa`（16 位通用寄存器保存/恢复），不要写成 `pushaw`——虽然 GAS 的 `pushaw` 等价于 `pusha`，但语义上 `pusha` 就已经表示 16 位操作数了，在 .code16 下更是如此。

**验证**：用 `objdump -d build/boot/stage2` 反汇编，在 `load_kernel_from_disk` 的反汇编中应该能看到循环结构——CMPW 检查 BX 与 832 的比较、SHLW 计算 buffer 段地址、多次 PUSHW/POPW 保存恢复 BX 和 BP、INT $0x13 调用。循环次数应该是 832/127 = 6.55 向上取整 = 7 次迭代（前 6 次各读 127 扇区 = 762，最后一次读 70 扇区 = 832）。

---

### Step 4: 更新内存布局常量

**目标**：确认并更新所有与加载地址相关的常量，确保 0x20000 加载方案在整个代码库中一致。

**设计思路**：内存布局的改动牵一发而动全身。你需要检查以下几个地方，确保它们都使用 0x20000 作为内核加载地址。首先是 `boot/common/boot.S` 里的 MINI_KERNEL_LOAD_PHYS 和 MINI_KERNEL_LOAD_SEG——这两个已经在 Step 3 改好了。其次是 `kernel/mini/linker.ld`——链接脚本里的物理加载地址（LMA）必须和 Bootloader 的加载地址一致，KERNEL_PHYS_BASE 应该是 0x20000。然后是 `scripts/build_image.sh`——构建脚本需要知道内核的大小限制，最大 416KB = 832 sectors，这个限制来自"0x20000 到 0x88000 之间有 416KB 空间，0x88000 到 0x90000（Protected Mode 栈）之间留 32KB 安全间隙"。

在 `boot/common/boot.S` 的常量区域，你应该有一段注释清晰地标注整个内存布局：Real Mode 栈在 0x9000~0x19000，内核加载区在 0x20000~0x88000（416KB），Protected Mode 栈在 0x90000。这个注释非常重要——三个月后你自己回来看代码的时候，会感谢今天写下这段注释的自己。

**踩坑预警**：如果你只改了 boot.S 的加载地址但忘了改 linker.ld 的 KERNEL_PHYS_BASE，会出现一个非常诡异的 bug：Bootloader 把内核正确加载到了 0x20000，但内核代码里的地址引用全部按 0x10000（旧值）计算——函数调用跳到错误的地址，全局变量读到错误的数据。更阴险的是，如果旧值和新值之间恰好没有严重冲突（比如旧值附近也是可写内存），内核可能"看起来"能跑一段但数据完全错乱。

**验证**：在 `boot/common/boot.S` 中确认 MINI_KERNEL_LOAD_PHYS = 0x20000，在 `kernel/mini/linker.ld` 中确认 KERNEL_PHYS_BASE = 0x20000，在 `scripts/build_image.sh` 中确认 MINI_KERNEL_MAX_BYTES = 425984（416 * 1024）。三个值必须对得上。

---

## 三、构建与运行

在项目根目录执行完整构建：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

构建完成后，确认以下文件存在：

- `build/kernel/mini/mini_kernel.bin`（flat binary，大小应该有几 KB 到几十 KB）
- `build/cinux.img`（磁盘镜像，大小至少 1MB）

然后用 QEMU 运行：

```
make run
```

QEMU 启动后按 `Ctrl+C` 退出，然后检查 debugcon 日志。预期看到字符序列 `OPL`。其中 `O` 表示磁盘读取完成（来自 boot.S 的 load_kernel_from_disk 成功路径，在 Real Mode 输出），`P` 表示 Protected Mode 切换成功（来自 stage2.S 的 pm_entry），`L` 表示 Long Mode 切换成功（来自 stage2.S 的 long_mode_entry）。

如果只看到了 `F`（没有 `P` 和 `L`），说明磁盘读取失败——`F` 是失败路径输出的字符，系统会直接 panic 而不会继续进入 Protected Mode。需要检查 DAP 参数、DL 寄存器是否为 0x80、以及 mini_kernel.bin 是否被正确写入磁盘镜像。

---

## 四、调试技巧

### 检查 flat binary 是否正确写入磁盘

这是 004B 最常见的问题来源。如果你怀疑写入磁盘的不是 flat binary，可以用以下方法确认：

```
xxd build/cinux.img | head -n 300 | grep "00002000"
```

LBA 16 对应的偏移是 0x2000（16 * 512 = 8192）。如果你在这一行看到 `7f45 4c46`（ELF magic），说明写入的是 ELF 文件而不是 flat binary——检查 build_image.sh 的第三个参数和 CMakeLists.txt 的 objcopy post-build 步骤。

### 用 GDB 验证内存中的内核数据

在 GDB 中可以在 `load_kernel_from_disk` 返回后检查 0x20000 处的数据：

```
(gdb) break load_kernel_from_disk
(gdb) continue
(gdb) finish
(gdb) x/16bx 0x20000
```

0x20000 处应该是 Mini Kernel 的 flat binary 开头。不应该看到 0x7F 0x45 0x4C 0x46（ELF magic）。如果看到了 ELF magic，和上面的问题一样——写入磁盘的是 ELF 文件。

### 排查栈冲突

如果你发现 Bootloader 在 `load_kernel_from_disk` 返回时崩溃（ret 跳到奇怪地址），很可能是栈冲突。在 GDB 中检查：

```
(gdb) break load_kernel_from_disk
(gdb) info registers ss sp
```

SS 应该是 0x0900，SP 应该是 0xFFFE 附近。物理栈顶 = 0x0900 << 4 + 0xFFFE = 0x19000。内核加载起始地址（MINI_KERNEL_LOAD_PHYS）必须大于 0x19000——如果你发现它被设成了 0x10000，那就是栈冲突了。

如果需要确认磁盘数据是否覆盖了栈区，可以在磁盘读取完成后检查 0x10000~0x19000 范围内的内容——如果这段区域有非零数据（且不是之前写入的），说明内核加载地址太低了。

---

## 本章小结

| 概念 | 要点 |
|------|------|
| BootInfo 结构体 | bootloader-kernel 交接协议，824 字节，packed + static_assert |
| MemoryMapEntry | 24 字节，E820 格式：base(8B) + length(8B) + type(4B) + acpi(4B) |
| Flat Binary | objcopy -O binary 去掉 ELF 元数据，直接加载到目标地址 |
| Flat Binary 代价 | 链接地址必须与加载地址完全匹配，不支持重定位 |
| 分块读取 | BIOS INT 13h 每次最多 127 扇区，循环 7 次读完 832 扇区 |
| 栈冲突 | Real Mode 栈 0x9000~0x19000，加载地址必须 > 0x19000，故用 0x20000 |
| 内存布局 | 栈 0x9000~0x19000，内核 0x20000~0x88000，PM 栈 0x90000 |
| Protected Mode | 过渡阶段，只设段寄存器和栈，不搬运数据 |
| BIOS 寄存器破坏 | INT 13h 破坏 BX/BP，必须 pushw 保存 |
| DAP DS:SI | INT 13h AH=42h 要求 DS:SI（不是 ES:SI）指向 DAP |
| 验证字符序列 | O -> P -> L（004B 预期） |
