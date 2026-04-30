# 004 加载小内核（完整加载篇） -- 从 4KB 到 416KB 的跨越

> 本章完成后的可见效果：QEMU debugcon 日志中出现 `OPL` 字符序列（O=disk read OK, P=Protected Mode, L=Long Mode）
>
> 前置要求：已完成 `004_boot_load_mini_kernel_A`，理解 E820 内存探测和 INT 13h 扩展读取的基本用法

---

## 导语

上一章我们用 E820 画好了内存地图，用 INT 13h 把 Mini Kernel 的 ELF header 前缀读到了 0x10000——但是说实话，4KB 的 ELF header 只够让 Bootloader "确认文件在磁盘上"，离真正把内核跑起来还差十万八千里。这一章我们要做的事情比较硬核：把加载量从 8 个扇区（4KB）一口气拉升到 832 个扇区（416KB），把完整的 Mini Kernel flat binary 从磁盘搬到内存里。听起来好像只是"多读几个扇区"的事情，但这里面藏着好几个能让 Bootloader 瞬间暴毙的坑——BIOS 单次读取上限、栈地址冲突、flat binary vs ELF 的取舍。我们一个一个来解决。

## 概念精讲

### BootInfo 结构体：Bootloader 和内核的交接协议

你可能会想：上一章不是已经把 E820 数据放到 0x5000、framebuffer 信息放到 0x6400 了吗？为什么还要再搞一个 BootInfo？这是因为"数据散落在固定地址"这个方案在小项目里勉强能用，但随着内核功能增长，你需要的信息会越来越多——内核入口地址、内核物理基址、内核大小、内存地图、framebuffer 参数……如果这些信息各自蹲在不同的固定地址上，内核启动时就得一个个去"寻宝"，地址改了一个就全部乱套。BootInfo 的思路是：定义一个统一的结构体，Bootloader 在跳转内核之前把所有信息打包进去，然后把结构体的指针通过寄存器（System V AMD64 ABI 用 RDI）传给内核。内核拿到指针后，一个结构体就能访问所有启动信息。

BootInfo 里的字段可以分成三类。第一类是内核自身的信息：入口点的虚拟地址（entry_point）、内核被加载到的物理基址（kernel_phys_base）、内核的实际大小（kernel_size）。第二类是显示相关的信息：framebuffer 的物理地址、宽度、高度、每行字节数（pitch）、色深（bpp）——这些来自上一章的 VESA 初始化。第三类是内存地图：一个计数字段（mmap_count）加上最多 32 条 E820 记录的数组，每条记录 24 字节（base 8字节 + length 8字节 + type 4字节 + acpi 4字节）。整个 BootInfo 大约 824 字节，放在物理地址 0x7000。

这里有一个非常重要的设计约束：BootInfo 的头文件会被 Bootloader（-m32 编译）和内核（-m64 编译）同时包含。32 位编译和 64 位编译下，结构体布局可能因为默认对齐规则不同而出现差异——比如 64 位编译器可能在某些字段之间插入填充字节。为了消除这种不确定性，所有字段都使用显式大小的类型（uint32_t、uint64_t），结构体整体加上 `__attribute__((packed))` 禁止编译器插入任何填充，并且用 `static_assert` 在编译时验证结构体大小。这样无论你用 32 位还是 64 位编译器，只要字段定义一样，内存布局就绝对一致。这个设计参考了 Linux 的 boot_params 和 Multiboot 规范的思路——虽然我们不需要它们那么复杂，但"固定结构体 + 寄存器传递指针"这个模式是经过实战验证的。

### Flat Binary vs ELF：简化到极致

上一章我们讨论了 ELF 格式的 Program Header，当时的设计是 Bootloader 解析 ELF header 后加载各个段。但到了真正动手实现的时候，你会发现一个尴尬的问题：在 Real Mode 下解析 ELF header 需要做 64 位算术运算（读取 8 字节的地址字段），而 Real Mode 的寄存器只有 16 位。虽然技术上可以通过多个寄存器拼接来实现，但代码会非常繁琐且容易出错。更关键的是，Mini Kernel 现在的链接地址是固定的（0x20000），不存在需要重定位的段——所有的段都会被加载到连续的地址空间里。在这种情况下，解析 ELF header 纯粹是在给自己找麻烦。

Flat binary 就是"纯二进制"——`objcopy -O binary` 把 ELF 可执行文件中的所有可加载段按地址顺序提取出来，去掉 ELF header、Program Header、Section Header 等所有元数据，生成一个纯粹的字节流。Bootloader 不需要任何解析，直接把这个字节流原封不动地读到目标物理地址就行了——因为链接脚本里指定的物理地址和 Bootloader 的加载地址是同一个值（0x20000），字节流被放到正确的位置后，代码里的地址引用天然就是对的。你可以把它类比成"搬家 vs 拆包"：ELF 加载是"拆包，看标签，放到对应房间"，flat binary 是"整个集装箱原封不动地卸到指定位置"。

当然 flat binary 也有代价：链接地址必须和加载地址完全匹配，不能做任何重定位。但对于我们 Mini Kernel 现阶段的需求来说，这个限制完全可以接受。等以后要加载"大内核"的时候，Mini Kernel 已经在 Long Mode 下运行了，有完整的 C++ 环境和 64 位算术能力，到时候再解析 ELF 也不迟。

### 分块磁盘读取：BIOS 的 127 扇区上限

上一章我们一次读了 8 个扇区，BIOS 处理起来毫无压力。但现在要读 832 个扇区（416KB），问题来了：BIOS INT 13h AH=42h 对单次读取的扇区数有上限，通常是 127 个扇区。这个限制的根源是 BIOS 内部的 DMA 缓冲区大小——超过 127 个扇区（约 65KB）的 DMA 传输在某些硬件上会出问题，所以 BIOS 厂商统一把这个值限制在 127。

解决方案是分块循环读取：每次读最多 127 个扇区，读完一批后更新参数读下一批，直到全部读完。具体来说，我们用一个寄存器跟踪"已经读了多少扇区"（BX），每轮循环先计算"还剩多少扇区"（总数减去已读），然后取"剩余扇区"和 127 的较小值作为本轮读取量。DAP 里的 LBA 需要每轮更新（起始 LBA + 已读扇区数），buffer 地址也需要每轮移动——因为每轮读出的数据必须紧接着上一轮的数据存放，这样最终在内存里才能形成连续的内核镜像。buffer 地址的计算公式是：基础段地址 + 已读扇区数乘以 32（因为每个扇区 512 字节，512 / 16 = 32 个"段单位"，而段地址每加 1 对应物理地址加 16 字节）。

这个循环逻辑本身不复杂，但有一个特别阴险的坑：BIOS 调用会破坏 BX 和 BP 寄存器的值。Intel SDM 没有明确列出"哪些寄存器会被 BIOS 破坏"，但实际测试表明，很多 BIOS 实现在 INT 13h 内部会使用 BX 和 BP 作为临时寄存器且不恢复。如果你用 BX 来跟踪已读扇区数却不保存，循环第二轮的 BX 就是一个垃圾值——要么读到错误的地址，要么 LBA 偏移完全错乱，甚至可能死循环。所以每次 BIOS 调用前必须 pushw 保存 BX 和 BP，调用后 popw 恢复。

### 内存布局与栈冲突：0x10000 为什么不行

上一章我们把内核 ELF header 加载到 0x10000，那时候只读 4KB 没出问题。但如果你把完整内核（最大 416KB）也加载到 0x10000，事情就炸了——因为 Real Mode 的栈就在附近。Stage2 的栈设置是 SS=0x0900、SP=0xFFFE，对应的物理地址范围是 0x9000 到 0x19000（栈从高地址往低地址增长，物理地址 = SS << 4 + SP = 0x9000 + 0xFFFE ≈ 0x19000，向下增长到 0x9000）。如果你把内核从 0x10000 开始加载 416KB，磁盘写入的数据会从 0x10000 一路写到 0x71000——而 0x10000 到 0x19000 这段区域正好覆盖了栈空间！

崩溃的机制是这样的：磁盘读取把数据写到了 0x10000~0x19000 的区域，覆盖了栈里保存的返回地址。当 `load_kernel_from_disk` 函数执行 `ret` 想返回 stage2 的时候，从栈里弹出的返回地址已经被磁盘数据覆盖了——CPU 跳到一个莫名其妙的地址，大概率是一个非法指令或者一个空指针，然后 triple fault 重启。这种 bug 的恐怖之处在于：它不是"读错了数据"，而是"数据写对了地方但那个地方恰好是栈"——所以你检查 0x10000 处的数据会发现内核确实被正确加载了，但函数就是回不去。

解决方案是把加载地址从 0x10000 改到 0x20000。判断标准是：加载地址必须大于栈顶物理地址（0x19000），0x20000 - 0x19000 = 0x7000 = 28KB 的安全间隙绰绰有余。内核最大 416KB，从 0x20000 加载到 0x88000，而 Protected Mode 的栈在 0x90000，中间还有 32KB 的安全距离。

### Protected Mode "无操作"：Real Mode 干完所有活

在上一章的设计里，Protected Mode 本来还需要做一些数据搬运的工作（比如解析 ELF header 后加载各个段）。但既然现在我们切换到了 flat binary，所有数据搬运（E820 探测 + 完整内核加载）都可以在 Real Mode 下完成——因为 Real Mode 有 BIOS 中断可用，可以方便地读磁盘。Protected Mode 阶段就只需要做三件事：设置段寄存器（DS、ES、FS、GS、SS 全部设为 32 位数据段选择子）、初始化栈（ESP = 0x90000）、输出一个 'P' 字符到 debugcon 表示"已进入保护模式"——然后直接进入 Long Mode 设置。Protected Mode 在这里纯粹是一个过渡阶段，不做任何实质性的数据操作。

## 动手实现

### Step 1: 创建 BootInfo 头文件

**目标**：创建 `boot/boot_info.h`，定义 Bootloader 和内核共用的数据结构。

**设计思路**：这个头文件是 Bootloader 和内核之间的"契约"——两边必须对数据布局有完全一致的理解。我们用显式大小的类型（stdint.h 的 uint32_t 和 uint64_t）而不是 int、long 这种平台相关的类型，加上 `__attribute__((packed))` 确保编译器不会偷偷插入填充字节。`static_assert` 在编译时验证结构体大小，如果有人不小心改了字段导致大小变化，编译直接报错而不是运行时数据错乱。

BootInfo 的字段布局你需要设计成这样：首先是内核相关的三个 uint64_t（entry_point、kernel_phys_base、kernel_size，共 24 字节），然后是 framebuffer 相关的四个字段（fb_addr 是 uint64_t，fb_width、fb_height、fb_pitch、fb_bpp 各是 uint32_t，共 24 字节），最后是内存地图（mmap_count 是 uint32_t，紧跟一个 uint32_t 的显式填充 _pad，然后是 mmap 数组——最多 32 个 MemoryMapEntry，每个 24 字节，共 768 字节）。MemoryMapEntry 的字段和 E820 返回格式完全一致：base（uint64_t）、length（uint64_t）、type（uint32_t）、acpi（uint32_t），共 24 字节，同样 packed。

关于 static_assert 的写法需要注意：这个头文件会被 C 文件（bootloader）和 C++ 文件（内核）同时包含。C11 用 `_Static_assert`，C++11 用 `static_assert`。你需要用 `#if defined(__cplusplus)` 来区分两种编译环境，分别使用对应的语法。两边的断言条件一样：MemoryMapEntry 必须是 24 字节，BootInfo 必须是 824 字节。

**踩坑预警**：如果你忘了加 `__attribute__((packed))`，64 位编译器可能会在 mmap_count 后面插入 4 字节填充来对齐后面的 mmap 数组（因为 mmap 数组的元素包含 uint64_t 字段，编译器想让数组起始地址 8 字节对齐）。这时候 BootInfo 的实际大小就会变成 828 字节而不是 824 字节——bootloader 按旧偏移写的字段，内核按新偏移读，所有数据全部错位。这种 bug 非常阴险，因为两边单独编译都不会报错，只有合在一起运行时才会出问题。所以 `static_assert` 是你最后的安全网。

**验证**：头文件创建后不需要单独构建验证，但可以确认文件路径为 `boot/boot_info.h`，并且 `static_assert` 的条件在 32 位和 64 位编译下都成立。后续 Step 5 会统一验证。

### Step 2: 改造磁盘读取为分块循环

**目标**：修改 `boot/common/boot.S` 中的 `load_kernel_from_disk` 函数，将一次读取 8 个扇区改为循环读取 832 个扇区（416KB），每次最多读 127 个扇区。

**设计思路**：函数的核心是一个循环。你需要三个关键变量：BX 记录"已读扇区数"（初始为 0），BP 保存"本轮要读的扇区数"，以及一个固定的 DAP 结构（每次循环更新里面的字段）。每轮循环的第一件事是计算本轮读取量——用总扇区数（832）减去已读扇区数（BX）得到剩余量，然后和 127 比较，取较小值存入 BP。然后构建 DAP：扇区数填 BP，buffer 段地址动态计算（MINI_KERNEL_LOAD_SEG + BX * 32，因为每读一个扇区段地址要前进 512/16 = 32），LBA 也动态计算（MINI_KERNEL_LBA + BX）。调用 INT 13h 后检查 CF，成功则 BX += BP 继续循环，失败则跳转到错误处理。循环退出条件是 BX >= MINI_KERNEL_SECTORS（832）。

关键的常量需要更新：MINI_KERNEL_SECTORS 从 8 改为 832，MINI_KERNEL_LOAD_PHYS 从 0x10000 改为 0x20000，MINI_KERNEL_LOAD_SEG 从 0x1000 改为 0x2000。新增常量 DISK_MAX_SECTORS_PER_CALL = 127 表示 BIOS 单次读取上限。buffer 段地址的动态计算是这样的：物理地址 = MINI_KERNEL_LOAD_PHYS + BX * 512，段地址 = 物理地址 >> 4 = MINI_KERNEL_LOAD_SEG + BX * (512/16) = MINI_KERNEL_LOAD_SEG + BX * 32。由于 BX 最大值是 832，BX*32 = 26624 = 0x6800，加上 MINI_KERNEL_LOAD_SEG = 0x2000 得到 0x8800，对应物理地址 0x88000——正好在 Protected Mode 栈（0x90000）之前。

寄存器保存方面有一个重要变化：函数开头用 `pusha`（不是 `pushaw`）保存 16 位寄存器。这里需要注意 protected mode 下才用 pushaw（不存在的，pusha 就是 16 位的），Real Mode 下 `pusha` 本身就是保存 16 位通用寄存器（AX、CX、DX、BX、SP、BP、SI、DI），效果和 `pushaw` 一样。在循环内部，每次 BIOS 调用前还需要额外 pushw 保存 BX 和 BP——因为 BIOS 会破坏它们。调用后先 popw 恢复 BP 和 BX，然后再把 BP 加到 BX 上更新已读扇区数。

成功时输出 'O'（0x4F）到 debugcon 表示磁盘读取完成，失败时输出 'F' 并跳转到 panic 错误处理。

**踩坑预警**：这是整个 tag 里坑最密集的地方。第一个坑：BIOS 会破坏 BX 和 BP。如果你不保存，循环第二轮 BX 就变成了垃圾值，buffer 地址和 LBA 全部乱套，轻则读到错误数据，重则直接卡死。第二个坑：INT 13h AH=42h 要求 DS:SI 指向 DAP——不是 ES:SI，也不是 ES:DI。上一章 E820 用的是 ES:DI，如果你按惯性写成 ES:SI，BIOS 读到的是一个空 DAP，返回一个无效参数错误。第三个坑：DAP 的 8 字节 LBA 字段中，高 32 位必须显式清零。不能假定 DAP 所在的 0x7B00 区域内存是干净的——QEMU 里可能碰巧是零，但真实硬件上几乎必然有残留数据，高位垃圾值会让 BIOS 尝试读取一个天文数字般的 LBA，直接报错。第四个坑：在 Real Mode 下应该用 `pusha`/`popa`（16 位通用寄存器保存/恢复），不要写成 `pushaw`——虽然 GAS 的 `pushaw` 等价于 `pusha`，但语义上 `pusha` 就已经表示 16 位操作数了，在 .code16 下更是如此。 Protected Mode 下才需要明确区分 16/32 位。

**验证**：用 `objdump -d build/boot/stage2` 反汇编，在 `load_kernel_from_disk` 的反汇编中应该能看到循环结构——CMPW 检查 BX 与 832 的比较、SHLW 计算 buffer 段地址、多次 PUSHW/POPW 保存恢复 BX 和 BP、INT $0x13 调用。循环次数应该是 832/127 = 6.55 向上取整 = 7 次迭代（前 6 次各读 127 扇区 = 762，最后一次读 70 扇区 = 832）。

### Step 3: 更新内存布局常量

**目标**：确认并更新所有与加载地址相关的常量，确保 0x20000 加载方案在整个代码库中一致。

**设计思路**：内存布局的改动牵一发而动全身。你需要检查以下几个地方，确保它们都使用 0x20000 作为内核加载地址。首先是 `boot/common/boot.S` 里的 MINI_KERNEL_LOAD_PHYS 和 MINI_KERNEL_LOAD_SEG——这两个已经在 Step 2 改好了。其次是 `kernel/mini/linker.ld`——链接脚本里的物理加载地址（LMA）必须和 Bootloader 的加载地址一致，KERNEL_PHYS_BASE 应该是 0x20000。然后是 `scripts/build_image.sh`——构建脚本需要知道内核的大小限制，最大 416KB = 832 sectors，这个限制来自"0x20000 到 0x88000 之间有 416KB 空间，0x88000 到 0x90000（Protected Mode 栈）之间留 32KB 安全间隙"。

在 `boot/common/boot.S` 的常量区域，你应该有一段注释清晰地标注整个内存布局：Real Mode 栈在 0x9000~0x19000，内核加载区在 0x20000~0x88000（416KB），Protected Mode 栈在 0x90000。这个注释非常重要——三个月后你自己回来看代码的时候，会感谢今天写下这段注释的自己。

**踩坑预警**：如果你只改了 boot.S 的加载地址但忘了改 linker.ld 的 KERNEL_PHYS_BASE，会出现一个非常诡异的 bug：Bootloader 把内核正确加载到了 0x20000，但内核代码里的地址引用全部按 0x10000（旧值）计算——函数调用跳到错误的地址，全局变量读到错误的数据。更阴险的是，如果旧值和新值之间恰好没有严重冲突（比如旧值附近也是可写内存），内核可能"看起来"能跑一段但数据完全错乱。

**验证**：在 `boot/common/boot.S` 中确认 MINI_KERNEL_LOAD_PHYS = 0x20000，在 `kernel/mini/linker.ld` 中确认 KERNEL_PHYS_BASE = 0x20000，在 `scripts/build_image.sh` 中确认 MINI_KERNEL_MAX_BYTES = 425984（416 * 1024）。三个值必须对得上。

### Step 4: 更新 Mini Kernel 入口和构建

**目标**：修改 Mini Kernel 的入口汇编文件、链接脚本和 CMakeLists.txt，配合 flat binary 加载方案。

**设计思路**：Mini Kernel 的入口需要做几件初始化工作，这些工作必须在 C++ 代码（mini_kernel_main）执行之前完成。入口汇编文件（`kernel/mini/arch/x86_64/boot.S`）的执行顺序是：首先关中断（cli），然后启用 SSE（设置 CR4 的 OSFXSR 和 OSXMMEXCPT 位，清 CR0.TS），接着设置栈指针（RSP 指向一个预定义的 8KB 栈顶），保存 BootInfo 指针（从 RDI 保存到一个全局变量，因为接下来清 BSS 会破坏 RDI），清零 BSS 段（从 __bss_start 到 __bss_end 填零），调用 C++ 全局构造函数（_init_global_ctors），最后把 BootInfo 指针从全局变量加载回 RDI 作为第一个参数调用 mini_kernel_main。

链接脚本（`kernel/mini/linker.ld`）的关键改动是物理地址（LMA）从 0x200000（2MB）改为 0x20000（128KB）。虚拟地址（VMA）保持 higher-half 设计：0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000。`objcopy -O binary` 只提取 LMA 对应的内容，所以 flat binary 的第一个字节就是 .text.start 段的 _start 入口。Bootloader 把这个 binary 加载到 0x20000 后，通过页表映射（identity map + higher-half map）就能正确执行。

CMakeLists.txt 需要一个 post-build 步骤：用 `objcopy -O binary` 把编译出的 ELF 可执行文件（mini_kernel）转换成 flat binary（mini_kernel.bin）。这一步是必须的——因为 Bootloader 不再解析 ELF 格式，它需要的是纯二进制文件。mini_kernel_common 库需要包含 `arch/x86_64/boot.S` 作为源文件，并且 include 路径要包含项目根目录（以便找到 `boot/boot_info.h`）。

在 `kernel/mini/main.cpp` 中，Mini Kernel 的入口函数接收一个 uint64_t boot_info_addr 参数（来自 RDI），但实际的 BootInfo 指针是从入口汇编保存的全局变量 `__boot_info_ptr` 中读取的（因为 boot.S 在清 BSS 之前就把 RDI 存好了）。当前阶段 main.cpp 的功能可以非常简单——用 outb 向 0xE9 端口输出一个 'M' 字符表示 Mini Kernel 已启动，然后进入 cli; hlt 死循环。后续 tag 会让它做更多事情。

**踩坑预警**：有一个非常容易被忽略的顺序问题——BootInfo 指针必须在清 BSS 之前保存。因为清 BSS 的操作会用 RDI 作为目标地址（rep stosb 指令），如果你先清 BSS 再保存 RDI，RDI 已经被改成了 __bss_start 的值，BootInfo 指针就丢了。另外，`__boot_info_ptr` 必须放在 `.data` 段而不是 `.bss` 段——虽然它初始值是 0 看起来可以放 .bss，但如果放 .bss 的话，清 BSS 操作本身没问题，但链接器可能把它和真正的未初始化变量混在一起，导致维护困难。

**验证**：构建完成后，确认 `build/kernel/mini/mini_kernel.bin` 文件存在且大小合理（不应该只有几个字节——如果只有 8 字节说明 CMakeLists.txt 没有正确包含 boot.S 和 main.cpp）。用 `xxd build/kernel/mini/mini_kernel.bin | head -5` 查看开头几个字节，应该看不到 0x7F 0x45 0x4C 0x46（ELF magic）——flat binary 没有 ELF header，开头直接就是机器码。

### Step 5: 更新构建脚本

**目标**：修改 `scripts/build_image.sh`，使用 mini_kernel.bin（flat binary）替代 mini_kernel ELF 文件，并加入大小限制检查。

**设计思路**：构建脚本的关键变化有两个。第一，写入磁盘镜像的文件从 mini_kernel ELF 变成了 mini_kernel.bin（flat binary）。ELF 文件有 header、section table 等元数据，实际加载到内存的是"去壳后的纯二进制"，但写入磁盘的应该是 flat binary——因为 Bootloader 不解析 ELF header，它直接把磁盘上的字节搬到内存里。如果磁盘上写的是 ELF 文件，那 0x20000 处放的就是 ELF header（0x7F 'E' 'L' 'F'...），而不是内核代码——CPU 跳过去执行 ELF header 字节的时候，会把这些随机字节当成指令，大概率直接 triple fault。

第二，加入 416KB 的大小限制检查。内核加载区是 0x20000 到 0x88000（416KB），如果 flat binary 超过这个大小就会覆盖 Protected Mode 栈区域（0x90000）。脚本在写入之前先用 stat 获取文件大小，和 MINI_KERNEL_MAX_BYTES（425984 = 416 * 1024）比较，超过就报错退出。错误信息应该包含实际的内存布局约束说明，方便排查。

Mini Kernel 写入磁盘的起始位置仍然是 LBA 16——前面 1 个扇区给 MBR，15 个扇区给 Stage2，LBA 16 开始就是 Mini Kernel 的地盘。写入参数用 `dd ... bs=512 seek=16`，意思是跳到第 16 个扇区开始写入。

**踩坑预警**：如果你发现 debugcon 输出了 0x7F 或者 'E' 'L' 'F' 这样的字节序列而不是内核代码的执行效果，99% 的概率是构建脚本还在用 ELF 文件而不是 .bin 文件写入磁盘。检查 build_image.sh 的第三个参数是不是 mini_kernel.bin 的路径。另一个容易出错的地方是 dd 命令的 seek 参数——必须是 16 而不是其他值，否则磁盘写入位置偏了，Bootloader 从 LBA 16 读出来的就不是内核。

**验证**：完整构建后，用 `xxd build/cinux.img | grep -A2 "00002000:"` 查看 LBA 16（偏移 0x2000 = 8192）处的内容。这里应该是 Mini Kernel 的 flat binary 开头——也就是 boot.S 编译出的机器码，而不是 0x7F 0x45 0x4C 0x46（ELF magic）。如果看到了 ELF magic，说明写入磁盘的是 ELF 文件而不是 flat binary。

## 构建与运行

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

QEMU 启动后按 `Ctrl+C` 退出，然后检查 debugcon 日志：

```
cat debug.log
```

预期看到字符序列 `OPL`。其中 `O` 表示磁盘读取完成（来自 boot.S 的 load_kernel_from_disk 成功路径，在 Real Mode 输出），`P` 表示 Protected Mode 切换成功（来自 stage2.S 的 pm_entry），`L` 表示 Long Mode 切换成功（来自 stage2.S 的 long_mode_entry）。

如果只看到了 `F`（没有 `P` 和 `L`），说明磁盘读取失败——`F` 是失败路径输出的字符，系统会直接 panic 而不会继续进入 Protected Mode。需要检查 DAP 参数、DL 寄存器是否为 0x80、以及 mini_kernel.bin 是否被正确写入磁盘镜像。

## 调试技巧

### 检查 flat binary 是否正确写入磁盘

这是 004B 最常见的问题来源。如果你怀疑写入磁盘的不是 flat binary，可以用以下方法确认：

```
xxd build/cinux.img | head -n 300 | grep "00002000"
```

LBA 16 对应的偏移是 0x2000（16 * 512 = 8192 = 0x2000）。如果你在这一行看到 `7f45 4c46`（ELF magic），说明写入的是 ELF 文件而不是 flat binary——检查 build_image.sh 的第三个参数和 CMakeLists.txt 的 objcopy post-build 步骤。

### 用 GDB 验证内存中的内核数据

在 GDB 中可以在 `load_kernel_from_disk` 返回后检查 0x20000 处的数据：

```
(gdb) break load_kernel_from_disk
(gdb) continue
(gdb) finish
(gdb) x/16bx 0x20000
```

0x20000 处应该是 Mini Kernel 的 flat binary 开头——也就是 boot.S 编译出的机器码。不应该看到 0x7F 0x45 0x4C 0x46（ELF magic）。如果看到了 ELF magic，和上面的问题一样——写入磁盘的是 ELF 文件。

### 排查栈冲突

如果你发现 Bootloader 在 `load_kernel_from_disk` 返回时崩溃（ret 跳到奇怪地址），很可能是栈冲突。在 GDB 中检查：

```
(gdb) break load_kernel_from_disk
(gdb) info registers ss sp
```

SS 应该是 0x0900，SP 应该是 0xFFFE 附近。物理栈顶 = 0x0900 << 4 + 0xFFFE = 0x19000。内核加载起始地址（MINI_KERNEL_LOAD_PHYS）必须大于 0x19000——如果你发现它被设成了 0x10000，那就是栈冲突了。

如果需要确认磁盘数据是否覆盖了栈区，可以在磁盘读取完成后检查 0x10000~0x19000 范围内的内容——如果这段区域有非零数据（且不是之前写入的），说明内核加载地址太低了。

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
