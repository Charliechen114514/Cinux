# 003-boot-long-mode · 进入 Long Mode

> 本章完成后的可见效果：QEMU 不崩溃，debugcon 输出 `L`（确认进入 64-bit long mode）
>
> 前置要求：已完成 `002_boot_gdt_protected`，理解保护模式切换流程和 GDT 基本概念

---

## 导语

上一章我们让 CPU 从实模式切到了保护模式，输出一个 `P` 字母宣告胜利。但说实话，2026 年了还在 32 位保护模式里写内核，多少有点考古的意思——寄存器只有 32 位宽，地址空间只有 4GB，各种特权级机制还停留在 80386 时代的设计。要写一个"现代"的操作系统，我们必须进入 Long Mode（64 位模式）。这一章的目标非常明确：从 32 位保护模式切换到 64 位 Long Mode，并通过 debugcon 输出一个 `L` 字符确认成功。听起来和上一章类似？没错，流程上确实有相似之处——都是设置一堆控制寄存器、做一次远跳转——但 Long Mode 切换比保护模式切换复杂得多，因为 x86_64 架构强制要求开启分页，而分页需要我们手工搭建一套四级页表结构。任何一步出错，等待你的仍然是那个老朋友：三重故障。

## 概念精讲

### 为什么必须进 Long Mode

保护模式下，通用寄存器最大 32 位（EAX、EBX...），地址线最多 32 根，寻址上限 4GB。对于现代系统来说这个限制太紧了——哪怕只是做一个教学内核，我们也希望能跑 64 位代码、用上 64 位寄存器（RAX、RSP 等），而且后续 C++ 内核的编译器默认就是 64 位输出，不进 Long Mode 的话内核代码根本没法跑。Long Mode 是 AMD 在 2003 年引入 x86_64 架构时定义的，后来被 Intel 采纳。你可以把它理解为保护模式的"全面升级版"——寄存器加倍到 64 位、地址空间理论上限提升到 256TB、指令集也做了扩展。

### Long Mode 是一个状态机

这一章最核心的认知是：进入 Long Mode 不是一个单一操作，而是一个严格有序的状态机。必须按固定顺序满足四个前置条件，缺一个、错一个顺序，CPU 都会直接三重故障。这四个条件和正确顺序是：第一，开启 CR4.PAE（Physical Address Extension，CR4 的 bit 5），让 CPU 支持 36 位（或更高）物理地址；第二，通过 MSR 0xC0000080（EFER 寄存器）设置 LME 位（bit 8），告诉 CPU "我要进入 Long Mode"；第三，把 CR3 指向一套合法的四级页表，因为 Long Mode 强制要求分页；第四，设置 CR0.PG（bit 31）开启分页，CPU 在这一步真正激活 Long Mode。开启分页之后，CPU 自动进入 Compatibility Mode（兼容模式），此时还在执行 32 位代码。只有当你做了一次远跳转、加载了一个 L=1 且 D=0 的代码段描述符到 CS 之后，CPU 才真正进入 64 位模式。整个状态机的推进是单向的、不可跳步的。

### 四级页表与 2MB 大页

Long Mode 使用四级页表来翻译虚拟地址：PML4（Page Map Level 4）→ PDPT（Page Directory Pointer Table）→ PD（Page Directory）→ PT（Page Table）。每一级有 512 个条目，每个条目 8 字节，所以一张表正好占一个 4KB 页。虚拟地址被拆成四个 9 位索引（分别用于 PML4、PDPT、PD、PT 的查找）加上一个 12 位页内偏移，总共 48 位有效地址。但这个四级结构有一个简化方案：在 Page Directory 这一级，如果把条目的 PS（Page Size）位设为 1，就可以直接映射一个 2MB 的大页，跳过 PT 层级。这对 Bootloader 阶段来说非常合适——我们只需要映射前几 MB 的物理内存让代码跑起来，用 2MB 大页只需要 3 张表（PML4、PDPT、PD），远比完整的四级结构简单。

### Identity Mapping

刚切换到 Long Mode 时，CPU 正在用物理地址执行代码。如果虚拟地址到物理地址的映射不是恒等的（即虚拟地址 0x1000 不对应物理地址 0x1000），CPU 就会立即找不到下一条指令，直接三重故障。所以我们必须做 Identity Mapping（恒等映射）：虚拟地址等于物理地址。这是一种"过渡方案"，后续内核初始化完成后可以建立更复杂的映射关系。

## 动手实现

### Step 1: 创建 Long Mode 模块文件

**目标**：创建 `boot/common/long_mode.S` 文件，作为 Long Mode 切换的独立模块。

**设计思路**：把 Long Mode 相关的函数（页表初始化、模式切换）从 stage2.S 中独立出来，放到 common 目录下作为单独的汇编文件。这样做的好处是 stage2.S 不会继续膨胀，而且 Long Mode 的初始化逻辑可以被其他启动路径复用。这个文件需要在头部用 `.code32` 标记，因为它是在保护模式下被调用的。

**实现约束**：文件需要定义两组常量。第一组是页表的物理地址布局——PML4 放在 0x1000、PDPT 放在 0x2000、PD 放在 0x3000，这是固定地址，因为 Bootloader 阶段没有动态内存分配。第二组是控制位掩码——PAGE_PRESENT（0x01）、PAGE_WRITABLE（0x02）、PAGE_LARGE（0x80）用于页表条目标志，MSR_EFER（0xC0000080）是 EFER 寄存器的 MSR 地址，EFER_LME（0x100）是 Long Mode Enable 位的值，CR4_PAE（0x20）是 PAE 位的掩码，CR0_PG（0x80000000）是分页使能位，CR0_PE（0x01）是保护模式使能位。

**踩坑预警**：EFER_LME 的值是 0x100（bit 8），绝对不能写成 0x1000。0x1000 对应的是 bit 12，即 AMD 的 SVME（Secure Virtual Machine Enable）位。如果写错了，CPU 不会报错——`wrmsr` 会很乖地设置 SVME 位而不是 LME 位——但当你随后开启分页时，CPU 发现 LME 没设却开了分页，直接三重故障。这个坑是我们在开发时实打实踩过的，GDB 里看到 EFER=0x1000 只有 SVME 没有 LME，定位了半天才发现是常量定义写错了一位。调试这种问题时，在 GDB 里用 `info registers` 查看 CR0、CR4 和 EFER 的值是最直接的排查手段。

**验证**：此时只需要确认文件被创建、常量定义正确即可，构建验证在后续步骤完成。

### Step 2: 实现页表初始化函数

**目标**：实现 `setup_page_tables` 函数，在固定物理地址 0x1000-0x3FFF 构建 3 级页表结构。

**设计思路**：页表初始化分两个阶段——先清零再填充。三张表（PML4、PDPT、PD）各占 4KB（即 1024 个 32 位双字），用 `rep stosl` 指令批量写入零值来清零。`rep stosl` 的意思是：把 EAX 的值（这里是 0）重复写入 ES:EDI 指向的内存地址 ECX 次，每次写入后 EDI 自动递增 4。所以把 EDI 设为目标地址、ECX 设为 1024、EAX 设为 0，一条指令就能清零一整页。

清零完成后，建立层级链接：PML4 的第 0 个条目写入 PDPT 的物理地址（0x2000）加上 Present+Writable 标志（0x03），PDPT 的第 0 个条目写入 PD 的物理地址（0x3000）加上同样的标志。这样 CPU 在做地址翻译时，从 CR3 找到 PML4，从 PML4[0] 找到 PDPT，从 PDPT[0] 找到 PD。

最后填充 PD 的前 4 个条目，每个条目映射一个 2MB 大页。第 i 个条目的物理基地址等于 i 左移 21 位（即 i × 2MB），标志位是 Present+Writable+Large（0x83）。具体来说：PD[0] 映射 0x00000000-0x001FFFFF（0-2MB），PD[1] 映射 0x00200000-0x003FFFFF（2-4MB），PD[2] 映射 0x00400000-0x005FFFFF（4-6MB），PD[3] 映射 0x00600000-0x007FFFFF（6-8MB）。总共 8MB 的恒等映射，对 Bootloader 阶段绰绰有余。

**实现约束**：函数不接收参数也不返回值，直接修改 0x1000-0x3FFF 处的内存。会修改 EAX、ECX、EDI 寄存器。注意在函数开头用 `cld` 指令清除方向标志位，确保 `stosl` 是递增写入而非递减。PD 条目的写入用一个循环实现：EDI 指向 PD 起始地址，ECX=4 控制循环次数，每次迭代计算 `i << 21 | 0x83` 写入当前条目，然后 EDI 前进 8 字节（因为每个条目是 64 位），EAX 自增。

**踩坑预警**：页表条目是 64 位的，但我们在 32 位保护模式下用 `movl` 只能一次写 32 位。对于 8MB 以下的物理地址，高 32 位是零，而我们已经用 `rep stosl` 把整页都清零了，所以低 32 位写入正确的值、高 32 位保持为 0 就行。但如果以后要映射 4GB 以上的物理地址，就必须额外写一条指令来设置高 32 位。

**验证**：在 GDB 中可以在 `setup_page_tables` 返回后检查页表内容。用 `x/8gx 0x1000` 查看 PML4，应该看到 PML4[0] = 0x2003（PDPT 地址 0x2000 + Present+Writable）。用 `x/8gx 0x2000` 查看 PDPT，PDPT[0] = 0x3003。用 `x/8gx 0x3000` 查看 PD，PD[0..3] 分别是 0x83、0x200083、0x400083、0x600083。

### Step 3: 实现模式切换函数

**目标**：实现 `enter_long_mode` 函数，执行从保护模式到 Long Mode 的 CPU 状态转换。

**设计思路**：这个函数参考 Intel SDM Vol.3A §10.8.5 的初始化要求执行五步操作。第一步，用 `movl` 把 PML4 的物理地址（0x1000）写入 CR3，让 CPU 知道页表在哪。第二步，读 CR4、用 OR 指令设置 bit 5（PAE）、写回 CR4——注意 CR4 不能直接做位运算，必须通过通用寄存器中转。第三步是整个流程中最容易出错的一步：用 `rdmsr` 读取 MSR 0xC0000080（EFER 寄存器）到 EDX:EAX，用 OR 设置 bit 8（LME），再用 `wrmsr` 写回去。第四步，用 `lgdt` 加载包含 64 位描述符的扩展 GDT 指针。第五步，读 CR0、同时设置 bit 31（PG）和 bit 0（PE）、写回 CR0，这一步 CPU 正式激活 Long Mode。

这五步的顺序不可调换。尤其是 PAE 必须在 LME 之前设置，LME 必须在 PG 之前设置。如果反过来——比如先开 PG 再开 LME——CPU 会因为你试图在没有 Long Mode 的情况下启用分页而直接 #GP。

**实现约束**：函数声明为 `.global`，不接收参数，不返回（因为最后是一条远跳转指令跳到 64 位代码）。`rdmsr` 指令把 MSR 的值读入 EDX:EAX（高 32 位在 EDX、低 32 位在 EAX），我们只修改 EAX 中的 LME 位，EDX 保持不变。`wrmsr` 的写入顺序和读取相反：从 EDX:EAX 写入 MSR。CR0 的写入值是 0x80000001，同时设置 PG 和 PE——注意不能只设 PG 而不设 PE，因为 PE 在上一章已经设过了，但 OR 操作不会清除已有的位，所以加上 PE 位是安全的做法。

**踩坑预警**：LGDT 加载的 GDT 指针必须是扩展后的版本（`gdt64_ptr`），而不是保护模式用的旧版 `gdt_ptr`。两者的区别在于基地址字段的宽度：32 位版用 `.long`（4 字节），64 位版需要 8 字节。Cinux 的做法是用两个 `.long`（低 32 位是 GDT 地址、高 32 位是 0）来拼出 64 位基地址，而不是用 `.quad`——因为在 32 位 ELF（elf32-i386）里使用 `.quad` 存储 GDT 地址会触发链接器的 64 位重定位错误。

**验证**：在 GDB 中，可以在 `enter_long_mode` 的每一步之间设置断点，用 `info registers cr0`、`info registers cr4`、`info registers efer` 查看寄存器状态。开启分页后，CR0 应该是 0x80000011（PE+PG），CR4 应该是 0x20（PAE），EFER 应该包含 LME 位（0x100 或更高）。

### Step 4: 修改 stage2 调用链和 GDT 扩展

**目标**：在 stage2.S 中调用 `setup_page_tables` 和 `enter_long_mode`，并扩展 GDT 添加 64 位代码段和数据段描述符。

**设计思路**：stage2.S 在保护模式初始化完成、输出 `P` 之后，不再进入停机循环，而是调用新的两个函数。调用顺序是先 `setup_page_tables` 再 `enter_long_mode`——因为 `enter_long_mode` 内部会把 CR3 指向页表，所以页表必须提前建好。GDT 需要在原有三个描述符（null、32 位代码段、32 位数据段）之后追加两个新的 64 位描述符。64 位代码段描述符的值是 0x00AF9A000000FFFF——Access Byte 是 0x9A（Present、DPL=0、Code、Executable、Readable），Flags nibble 是 0xA（L=1、D=0、G=1），这意味着这是一个 64 位代码段。L=1 且 D=0 是让 CPU 进入真正 64 位模式的关键组合——如果 L=0 或 D=1，即使开了 Long Mode，CPU 也只会在兼容模式下运行。64 位数据段描述符的值是 0x008F92000000FFFF，Access Byte 0x92（Present、DPL=0、Data、Writable）。

新的 64 位代码段选择子是 0x18（GDT 偏移 24 字节 = 第 4 个描述符，index = 3），64 位数据段选择子是 0x20（GDT 偏移 32 字节 = 第 5 个描述符，index = 4）。`gdt64_ptr` 是新的 64 位 GDT 指针，格式是 2 字节 Limit + 4 字节低地址 + 4 字节高地址（全零），在 `enter_long_mode` 中被 `lgdt` 加载。

同时需要在 stage2 的 CMakeLists.txt 中把 `boot_longmode` 目标的对象文件链接进 stage2——在 `add_executable(stage2 ...)` 中追加 `$<TARGET_OBJECTS:boot_longmode>`。

**验证**：用 `objdump -d build/boot/stage2` 反汇编，确认 stage2 的 pm_entry 之后有 `call setup_page_tables` 和 `call enter_long_mode`。用 `readelf -s build/boot/stage2 | grep gdt` 确认 GDT 符号的链接地址正确。

### Step 5: 实现 64 位入口点

**目标**：在 stage2.S 中添加 `.code64` 段的 `long_mode_entry` 入口点，完成 64 位环境初始化。

**设计思路**：远跳转 `ljmp $0x18, $long_mode_entry` 执行后，CPU 进入 64 位模式，执行流到达 `long_mode_entry` 标签。此时 CS 已经通过远跳转刷新为 64 位代码段选择子（0x18），但 DS、ES、FS、GS、SS 这些数据段寄存器还残留着保护模式的旧值。我们需要把它们全部重载为 64 位数据段选择子（0x20）——和上一章保护模式初始化时的逻辑一模一样，只不过现在用的是 64 位描述符的选择子。接着设置 64 位栈指针：用 `movabsq` 指令把一个 64 位立即数（0x90000）加载到 RSP。这里用 `movabsq` 而不是 `movl` 是因为栈指针现在是 64 位的，必须用 64 位操作来设置。最后输出字符 `L`（0x4C）到 debugcon 端口 0xE9 作为验证——如果 `debug.log` 里出现了 `L`，就意味着整个 Long Mode 切换流程全部成功。

**实现约束**：`long_mode_entry` 需要声明为 `.global`，因为 `enter_long_mode` 中的 `ljmp` 需要引用这个符号。`.code64` 指令必须紧贴在 `long_mode_entry` 标签之前，确保从这里开始生成 64 位指令编码。`movabsq` 是 GAS 中加载 64 位立即数到寄存器的助记符，不是 `movq`——`movq` 只能接受 32 位符号扩展立即数。

**验证**：构建并运行 QEMU，检查 `debug.log` 文件内容。如果里面同时出现 `P`（保护模式）和 `L`（Long Mode），说明整个启动链条从实模式到保护模式再到 Long Mode 全部正确。如果只看到 `P` 没有 `L`，说明 Long Mode 切换过程中出了问题，大概率是 EFER_LME 位没设对或者页表结构有误。

## 构建与运行

构建命令和之前一样，在项目根目录执行 CMake 构建。QEMU 的启动参数也不需要变化——debugcon 配置在之前的 tag 中已经设好了。运行后检查 `debug.log` 文件：

```
cat debug.log
```

预期输出是两个字符：先 `P`（保护模式切换成功，来自 tag 002），然后 `L`（Long Mode 切换成功）。如果什么都没有或者 QEMU 反复重启，说明切换过程中某一步出了问题。

## 调试技巧

### 用 GDB 检查页表

在 GDB 中连接 QEMU（`target remote :1234`），加载 ELF 文件（`file build/boot/stage2`），在 `setup_page_tables` 的返回处设断点，然后检查三张页表的内容：

```
(gdb) x/8gx 0x1000
(gdb) x/8gx 0x2000
(gdb) x/8gx 0x3000
```

PML4[0] 应该是 0x2003，PDPT[0] 应该是 0x3003，PD[0..3] 应该是 0x83、0x200083、0x400083、0x600083。如果哪个值不对，回头检查清零和填充的逻辑。

### 检查控制寄存器状态

在 `enter_long_mode` 的关键步骤之间设置断点，逐步查看 CR0、CR4、EFER 的变化：

```
(gdb) info registers cr0
(gdb) info registers cr4
(gdb) info registers efer
```

开启分页后的正确状态应该是 CR0 = 0x80000011（PE+PG），CR4 = 0x20（PAE），EFER 包含 LME（0x100）。如果 EFER 里看到 0x1000 而没有 0x100，说明 LME 位设错了——大概率是常量定义写成了 0x1000 而不是 0x100。

### 常见三重故障排查

按出现频率排序：第一，EFER_LME 位定义错误（0x1000 vs 0x100）——这是我们实打实踩过的坑，在 GDB 里查 EFER 值就能确认。第二，页表结构不正确——PML4/PDPT/PD 的链接关系写错了、PD 条目的标志位不对、或者 2MB 对齐的地址算错了。第三，GDT 描述符格式错误——64 位代码段的 L 位没设为 1、D 位没清为 0。第四，`lgdt` 加载了错误的 GDT 指针——用了 32 位版的 `gdt_ptr` 而不是 64 位版的 `gdt64_ptr`。第五，GDT 指针中用了 `.quad` 导致 32 位 ELF 链接失败或地址截断。

## 本章小结

| 概念 | 要点 |
|------|------|
| Long Mode 状态机 | PAE → LME → CR3 → PG，严格有序 |
| 四级页表 | PML4 → PDPT → PD → PT，每级 512 条目 × 8 字节 = 4KB |
| 2MB 大页 | PD 条目设 PS=1（0x80），跳过 PT 层级 |
| Identity Mapping | 虚拟地址 = 物理地址，Bootloader 阶段必须 |
| EFER.LME | MSR 0xC0000080 的 bit 8，值 0x100，不是 0x1000 |
| 64 位 GDT 描述符 | L=1, D=0，代码段值 0x00AF9A000000FFFF |
| 远跳转选择子 | 0x18（64 位代码段），触发真正 64 位模式 |
| GDT64 指针 | .long + .long 拼接 64 位基地址，避免 .quad 重定位 |
| 验证输出 | `outb 'L'` 到 debugcon 0xE9，debug.log 应出现 `L` |
