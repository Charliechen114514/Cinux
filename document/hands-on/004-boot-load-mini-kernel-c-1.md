# 004-C 动手篇（上）：BootInfo 填充与高半核跳转

> 本章完成后的可见效果：debugcon 日志中在 `OPL` 之后出现 `J` 字符，紧接着出现 `123G4===CPPGC1V123B===END`，表示 Bootloader 成功填充 BootInfo 并跳转到 Mini Kernel 的 C++ 入口
>
> 前置要求：已完成 `004_boot_load_mini_kernel_B`，理解 Mini Kernel 的磁盘加载和 Real Mode 下的 E820/VESA 信息采集

---

## 导语

上一章我们在 Real Mode 下把 Mini Kernel 的完整二进制从磁盘读到了物理地址 0x20000，然后一路切进 Long Mode 打出了那个熟悉的 `L`。但说实话，此时此刻 CPU 跑的还是 Bootloader 的代码——内核虽然已经在内存里躺着了，可我们连碰都没碰它。更关键的是，之前 E820 探测的内存映射、VESA 获取的帧缓冲信息，全都散落在 0x5000 和 0x6400 这两块低内存区域，内核压根不知道它们的存在。

这一章我们要做的事情，就是在这两块碎片和内核之间搭一座桥。具体来说：进入 Long Mode 之后，我们把所有零散的引导信息打包成一个固定格式的 BootInfo 结构体，写到物理地址 0x7000；然后在页表里新增一组高半核映射（Higher-Half Mapping），让内核的虚拟入口地址 0xFFFFFFFF80020000 指向和物理地址 0x20000 相同的那块物理内存；最后把 BootInfo 的地址塞进 `%rdi`，一记 `jmp *%rax` 跳过去，从此 Bootloader 功成身退，内核接管一切。

听起来逻辑很干净对吧？但魔鬼全在细节里——BootInfo 的结构布局要和 C 头文件严格对齐、高半核的 PML4/PDPT 索引要算对、从 VESA 临时缓冲区搬运帧缓冲字段时字节宽度和偏移差一拍都不行。而且这一章还涉及到一个跨编译环境的关键问题：BootInfo 的头文件同时被 Bootloader（32 位 C）和内核（64 位 C++）include，两边对 `static_assert` 的关键字叫法都不一样——这种细节漏了编译直接炸。下面我们一步一步来。

---

## 低 1MB 内存地图

动手之前先快速回顾一下当前的低 1MB 内存布局，因为 BootInfo 放在哪里、数据从哪里来，完全取决于这个布局。从 0x5000 到 0x7C00 之间的区域已经相当拥挤了——E820 缓冲区占了 0x5000 起始的一段，VESA 帧缓冲信息在 0x6400，DAP 在 0x7B00，MBR 在 0x7C00，Stage2 从 0x8000 开始。现在我们要在这个夹缝里再塞一个 824 字节的 BootInfo，选位置的时候必须避开所有已占用的区域。选定的位置是 0x7000——它到下一个占用区域（0x7B00 的 DAP）之间有约 2KB 的空间，而 BootInfo 只需要 824 字节（约 0x338），放得下。

---

## 概念精讲

### BootInfo：Bootloader 和内核之间的"交接清单"

你可以把 BootInfo 想象成一份搬家时留给新房客的清单——上面写着"电视机在哪、电表在哪、仓库有多大、哪些房间能用"。Bootloader 是旧房客，跑完就走了；内核是新房客，进门第一件事就是看这份清单，搞清楚环境状况，然后才开始干活。

这份清单放在物理地址 0x7000，总共 824 字节，`__attribute__((packed))` 压缩过，没有任何填充字节。它的前三个字段是内核自身的信息：入口点的虚拟地址（高半核地址，8 字节）、物理加载基地址（8 字节）、以及内核大小（8 字节）。紧接着是帧缓冲区的六个字段——物理地址（8 字节）、宽度（4 字节）、高度（4 字节）、每行字节数 pitch（4 字节）、色深 bpp（4 字节）。然后是内存映射：条目数量（4 字节）、一个 4 字节对齐填充、最多 32 条 E820 记录，每条 24 字节（base 8 字节 + length 8 字节 + type 4 字节 + acpi 4 字节），总计 768 字节。

算一下：3 个 uint64_t 加 fb_addr 是 32 字节，5 个 uint32_t 是 20 字节，mmap_count 加 _pad 是 8 字节，32 条 mmap 是 768 字节——加起来正好 824。我们在头文件里放一个 `static_assert(sizeof(BootInfo) == 824)` 作为编译期断言，谁改了结构体编译直接报错，而不是跑到运行时数据错位才抓狂。

这里有一个 C/C++ 兼容性的小坑值得提一嘴：`static_assert` 在 C++ 里是关键字，但在 C 里叫 `_Static_assert`。因为 `boot_info.h` 这个头文件同时被 Bootloader 的汇编预处理和内核的 C++ 代码包含，所以需要用 `#if defined(__cplusplus)` 做条件编译，两边各用各的写法。别小看这个细节——如果你只写了 `static_assert`，用纯 C 编译 Bootloader 的时候直接就炸了。

### 高半核映射：为什么内核要住在地址空间的"阁楼"里

x86-64 的 64 位虚拟地址并不是真的 64 位都用上了。硬件只实现了 48 位的有效地址，剩余的高 16 位（bit 48 到 bit 63）必须全是 0 或全是 1——这叫"规范地址"（Canonical Address）。全 0 的范围是 0x0000000000000000 到 0x00007FFFFFFFFFFF，这是用户空间；全 1 的范围是 0xFFFF800000000000 到 0xFFFFFFFFFFFFFFFF，这是内核空间。我们选的内核基址 0xFFFFFFFF80000000 就在这个内核空间的上半段，所以叫"高半核"（Higher-Half Kernel）。

为什么要费这个劲？最直接的好处是：内核占据高地址空间后，整个低地址区域从 0 开始的一段就可以完整地给用户程序用，不需要在每次创建用户进程时费心绕开内核占用的那段低地址。Linux、Windows、macOS 的内核全都是高半核设计——这不是偶然，而是地址空间管理的最佳实践。

在我们的四级页表结构里（PML4 -> PDPT -> PD -> Page），虚拟地址 0xFFFFFFFF80020000 的索引拆解如下：PML4 索引取 bit 47:39，也就是 511（0x1FF）；PDPT 索引取 bit 38:30，算出来是 510（0x1FE）；PD 索引取 bit 29:21，得 0（0x000）。所以我们需要在 PML4 的第 511 个条目里填入 PDPT 的物理地址，在 PDPT 的第 510 个条目里填入 PD 的物理地址。PD 的第 0 个条目已经在之前设置了——它用 2MB 大页映射了物理地址 0~2MB 的范围，而 0x20000 正好落在这个范围内。这样一来，身份映射（0x20000 -> 0x20000）和高半核映射（0xFFFFFFFF80020000 -> 0x20000）指向同一块物理内存，同一份内核代码可以通过两套地址访问到。Bootloader 跳转用高半核地址，内核从此就"住进了阁楼"。

Intel SDM Vol. 3A 第 4 章（Paging）对四级页表结构的索引拆解有完整的说明，Virtual Address 的 bit 划分和每一级页表的物理布局都写得非常清楚。

值得一提的是这里有一个规范细节需要注意：x86-64 的虚拟地址必须是 canonical address，即 bits 63:48 必须是 bit 47 的符号扩展。`0xFFFFFFFF80020000` 的 bit 47 = 1，所以 bits 63:48 全部是 1——这是合法的 canonical address。如果你尝试访问一个非 canonical 地址（比如 `0x0000800000000000`，bit 47 = 0 但 bit 48 = 1），CPU 会直接触发 General Protection Fault，连页表查询都不会做。这个约束来自 x86-64 架构的虚拟地址设计——Intel SDM Vol. 3A Section 3.3.7 对此有详细说明。

---

## 动手实现

### Step 1: 更新 boot_info.h——C/C++ 兼容的 static_assert 和正确的 BootInfo 尺寸

**目标**：确保 `boot/boot_info.h` 这个头文件既能被 C++ 编译器（编译内核），又能被 C 编译器（编译 Bootloader 相关工具）正常处理，同时保证 BootInfo 的尺寸断言值是正确的 824。

**设计思路**：上一章定义 BootInfo 的时候可能 `static_assert` 只写了 C++ 的形式，或者 BootInfo 的 size 计算值不是 824。现在我们要修复这两个问题。`static_assert` 是 C++11 引入的关键字，对应的 C11 标准写法是 `_Static_assert`，两者语法略有差异（C 版本需要把字符串放在第二个参数位置）。通过 `#if defined(__cplusplus)` 做分支选择就能两边都照顾到。BootInfo 的 size 之前可能用了表达式（比如 `24 + 32 + 8 + ...`），这种写法在有人加字段的时候容易漏算，不如直接写死 `824` 这个最终值——如果有人改了结构体，`static_assert` 会立即报错。

**实现约束**：修改位置在 `boot/boot_info.h` 文件底部。`MemoryMapEntry` 的断言也要加 C/C++ 兼容。不要改变结构体字段的顺序和类型——Bootloader 的汇编代码是按固定偏移量来填写 BootInfo 的，改了字段就全乱套了。

**验证**：编译 Bootloader 和内核都不应该报错。如果你怀疑结构体布局有问题，可以在内核代码里打印 `sizeof(BootInfo)`，确认输出是 824。

### Step 2: 在 long_mode.S 中新增高半核页表条目

**目标**：在 `setup_page_tables` 函数末尾，让 PML4[511] 指向已有的 PDPT，PDPT[510] 指向已有的 PD，建立 0xFFFFFFFF80000000 起始的高半核映射。

**设计思路**：我们在 003 章已经建立了身份映射（PML4[0] -> PDPT[0] -> PD，映射 0 到 2MB 的物理内存）。高半核映射需要复用同一套 PDPT 和 PD——区别仅在于从 PML4 的不同索引进入。具体来说：PML4[511] 里写入"PDPT 物理地址 | 0x03"（Present + Writable），PDPT[510] 里写入"PD 物理地址 | 0x03"。PD[0] 已经在身份映射阶段设好了，它用 2MB 大页映射了物理地址 0~2MB 的范围，而 0x20000 正好落在这个范围内。现在通过 PML4[511] -> PDPT[510] -> PD[0] 这条路径，访问 0xFFFFFFFF80020000 就等于访问物理 0x20000。两条映射指向同一块物理内存，零额外开销。

你需要确认 `PML4_PHYS_ADDR`、`PDPT_PHYS_ADDR`、`PD_PHYS_ADDR` 这三个常量已经在之前的代码中定义好了（它们的值取决于你把页表放在了哪块低内存区域）。AT&T 汇编语法下，往 PML4 的第 511 个条目（偏移 511*8=4088 字节）写入一个 32 位值，用的是 `movl %eax, PML4_PHYS_ADDR + (511 * 8)`。PDPT[510] 同理，偏移是 510*8=4080 字节。

**踩坑预警**：这里只用了 32 位的 `movl` 来写页表条目，因为我们运行在 32 位兼容模式下（setup_page_tables 是在进 Long Mode 之前调用的），页表物理地址都在 1MB 以内，高 32 位全是 0。如果你手滑用了 `movq`，在某些汇编器配置下会报错或者生成错误的操作码。另外，条目的 flags 只需要 Present(0x01) + Writable(0x02) = 0x03，不需要设置 Accessed/Dirty 之类的位——那些是 CPU 在运行时自动维护的。

**验证**：这一步暂时没有独立的验证手段，需要等 Step 4 跳转到高半核地址时才能确认映射是否正确。如果你加了错误的映射，跳转时 CPU 会触发 Triple Fault（因为页表找不到对应的物理页），QEMU 会重启。

### Step 3: 在 stage2.S 中填充 BootInfo

**目标**：在 `long_mode_entry` 标签之后（就是输出 `L` 字符之后的那段代码），把 BootInfo 的各个字段写到物理地址 0x7000。

**设计思路**：此时我们已经处于 64 位 Long Mode，所有 64 位寄存器都能用了。填写 BootInfo 的过程本质上是一连串的内存写入——先把 `%rdi` 设为 0x7000（BootInfo 的基地址），然后按偏移量依次填入各个字段。数据来源有两处：帧缓冲信息从物理地址 0x6400 读取（这是之前 VESA 初始化时保存的位置），内存映射从物理地址 0x5000 读取（E820 的输出位置）。

填写顺序和偏移量必须和 `boot_info.h` 的结构体定义严格一致。entry_point 是偏移 0（8 字节），写入高半核虚拟地址 0xFFFFFFFF80020000；kernel_phys_base 是偏移 8（8 字节），写入 0x20000；kernel_size 是偏移 16（8 字节），写入内核的最大尺寸（比如 0x68000 = 416KB）。帧缓冲区域稍微复杂一点：0x6400 处存储的格式是 fb_addr（8 字节）在偏移 0，fb_pitch（4 字节）在偏移 8，fb_width（2 字节）在偏移 12，fb_height（2 字节）在偏移 14。但 BootInfo 里的字段顺序是 fb_addr、fb_width、fb_height、fb_pitch、fb_bpp——顺序不一样！所以你需要先从 0x6400 读 fb_addr 写到 BootInfo 偏移 24，再读 pitch 写到偏移 40，读 width（注意要做 16 位到 32 位的零扩展）写到偏移 32，读 height 写到偏移 36。fb_bpp 直接硬编码为 32，写到偏移 44。

内存映射部分：从 0x5000 读前 4 字节作为 mmap_count 写到 BootInfo 偏移 48，然后从 0x5004 开始复制最多 768 字节（32 条 x 24 字节）到 BootInfo 偏移 56。这里的逐字节拷贝循环可以用 `%rsi` 指向源地址、`%rdx` 作为目标偏移、`%rcx` 作为源偏移、`%r8` 作为剩余计数——每次读一个字节写到目标，四个计数器各自递增或递减，直到 `%r8` 归零。

**踩坑预警**：VESA 保存的 width 和 height 是 16 位值（VBE Mode Info 结构就是这样定义的），但 BootInfo 里它们是 32 位的。你需要用零扩展指令（AT&T 语法下是 `movzwq`，把 16 位零扩展到 64 位然后截取低 32 位写入）来读取它们，不能直接用 `movl`，否则高位可能带上垃圾数据。另外，帧缓冲字段在 BootInfo 里的排列顺序和 0x6400 处的 VESA 缓冲区格式不一样——pitch 在 VESA 缓冲区是第二个字段，但在 BootInfo 里是第四个。照着 VESA 的顺序抄到 BootInfo 里就会写到错误的偏移，后续内核拿到的帧缓冲参数全是乱的。

**验证**：暂时没有独立的运行验证，需要配合 Step 4 的跳转一起验证。但你可以通过 GDB 在跳转前检查 0x7000 处的内存内容——比如 `x/gx 0x7000` 应该看到 entry_point 为 0xFFFFFFFF80020000，`x/gx 0x7008` 应该看到 0x20000。

### Step 4: 跳转到 Mini Kernel 的高半核入口

**目标**：在 BootInfo 填充完成后，向 debugcon 输出 `J` 字符作为"即将跳转"的标记，然后把 BootInfo 地址放进 `%rdi`（System V AMD64 ABI 的第一个参数寄存器），把内核入口地址放进 `%rax`，执行间接跳转。

**设计思路**：这是 Bootloader 使命的终点。按照 System V AMD64 ABI 的调用约定，函数的第一个参数通过 `%rdi` 传递。虽然这里不是标准的函数调用（而是 `jmp` 跳转），但我们遵循同样的约定——把 BootInfo 的物理地址 0x7000 放进 `%rdi`，让内核入口代码知道从哪里读取引导信息。内核入口地址是 0xFFFFFFFF80020000（高半核地址），放进 `%rax` 后用 `jmp *%rax` 做间接跳转。这里用 `jmp` 而不是 `call`，是因为我们不打算返回——Bootloader 的生命到此结束。跳转之后 bootloader 的代码就永远不会回来了，所以用 `jmp` 在语义上更准确——`call` 会把返回地址压栈白白浪费 8 字节。

这个跳转跨越了虚拟地址空间：从 identity mapping 区域（bootloader 的代码还在低地址执行）跳到 higher-half 区域（内核的 `_start` 在 0xFFFFFFFF80020000）。因为两条映射指向同一套物理页表条目，CPU 的 TLB 能够正确地翻译这个地址，跳转不会触发 Page Fault。

输出 `J`（0x4A）到 debugcon 是为了在调试日志中留下明确的分界点。如果 QEMU 在 `J` 之后立刻 Triple Fault 重启了，说明要么高半核页表映射有问题，要么内核入口代码本身有 bug。如果 `J` 之后什么输出都没有（QEMU 挂起），说明内核入口可能卡在了某个死循环或者 `hlt` 上。有了这个 `J` 标记，排查范围就缩小了很多。

**踩坑预警**：这里有一个非常容易犯的错误——`jmp *%rax` 是 AT&T 语法的间接跳转，星号不能省。如果你写成了 `jmp %rax`，汇编器会把它解释成跳转到 `%rax` 里的值所代表的相对偏移，而不是跳转到 `%rax` 指向的地址。这两种解释的结果天差地别——前者大概率跳到了一个完全错误的位置。另外，跳转之前别忘了再次把 `%rdi` 设为 0x7000——前面填充 BootInfo 的过程中你可能改了 `%rdi`（毕竟你拿它当目标基地址用了），需要在跳转前重新设置。

**验证**：这是本篇最重要的验证步骤。完整构建并运行 QEMU 后，debugcon 日志应该出现字符序列 `OPLJ`——`O` 是 MBR 加载 Stage2，`P` 是保护模式切换，`L` 是 Long Mode 进入，`J` 是跳转到 Mini Kernel。如果 `J` 之后紧跟着 `1`、`2`、`3`、`4` 这几个字符，说明内核入口代码（boot.S）正在执行——那是下一篇的内容了。

## 构建配置变更：debugcon 全局启用

在动手构建之前，有一个小但关键的配置变更需要先做。`cmake/qemu.cmake` 需要把 debugcon 相关的 QEMU 参数从 `QEMU_DEBUG_FLAGS`（只在 debug 模式启用）移到 `QEMU_COMMON_FLAGS`（所有模式都启用）。

原因是这样的：之前 debugcon 只在 `make run-debug` 下启用，正常 `make run` 下是不启用的。但从 004C 开始，内核的 `boot.S` 和 `main.cpp` 都依赖 debugcon 输出来验证执行状态——`OPLJ123G4===CPPGC1V123B===END` 这串字符全部通过 `outb` 写到 0xE9 端口。如果正常模式下不启用 debugcon，`outb %al, $0xE9` 会变成一个空操作，你什么都看不到。

具体来说，`QEMU_COMMON_FLAGS` 需要包含 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9` 这两个参数。`-global isa-debugcon.iobase=0xe9` 把 debugcon 设备的 I/O 端口绑定到 0xE9，QEMU 的 isa-debugcon 设备默认端口就是 0xE9，但显式指定可以避免未来的默认值变更导致问题。`-debugcon file:debug.log` 指定 debugcon 输出写到 `debug.log` 文件。

## 构建与运行

配置变更完成后，在项目根目录执行完整构建并运行。构建命令和前面几章一样——CMake 配置、编译、`make run`。

QEMU 启动后，debugcon 日志（`debug.log` 文件）应该包含以下字符序列：

```
OPLJ123G4===CPPGC1V123B===END
```

其中 `OPLJ` 是本篇关注的部分——`J` 的出现意味着 Bootloader 成功填充了 BootInfo 并完成了高半核跳转。后面从 `1` 开始的字符是 Mini Kernel 的入口代码和 C++ 运行时产生的，下一篇会详细讲解。

如果你只看到了 `OPL` 而没有 `J`，说明跳转代码没有执行到。检查 `stage2.S` 中 `long_mode_entry` 之后的代码是否被正确汇编和链接。如果看到了 `J` 但之后 QEMU 立刻重启（Triple Fault），说明高半核页表映射可能有问题——这时候就需要动用下面的调试技巧了。

## 调试技巧

这一章涉及三个容易出错的环节：页表映射、BootInfo 字段填充、跳转本身。下面按场景给出排查思路。

### 场景 1：OPL 之后没有 J

这是最常见的问题——BootInfo 填充代码或跳转代码没有执行到。可能的原因包括：`long_mode_entry` 之后的代码没有被正确汇编（检查链接脚本是否包含了 stage2.S 的 Long Mode 部分）、BootInfo 填充过程中某条指令触发了异常（比如访问了无效地址）。

排查方法是在 GDB 中在 `long_mode_entry` 后面设断点，单步执行 BootInfo 填充代码，看看卡在哪一步。

### 场景 2：J 之后 Triple Fault

这说明高半核页表映射有问题。在 GDB 中可以在跳转前检查页表内容：

```
(gdb) x/gx 0x1000 + 511*8
```

PML4 的物理地址是 0x1000，第 511 个条目的偏移是 511*8=4088。你应该看到一个有效的 PDPT 物理地址（0x2000）加上 0x03 标志位。PDPT[510] 同理，检查 0x2000 + 510*8 处是否为 PD 物理地址（0x3000）加 0x03。如果那里是 0 或者是一个看起来不像物理地址的值，说明 `setup_page_tables` 的那段新增代码没有正确执行。

另外一个容易忽视的点是：页表条目是 64 位的，而我们在 32 位模式下只写了低 32 位。如果你的汇编器没有自动把高 32 位清零，页表条目的高半部分可能带着垃圾数据。实际代码中需要显式用 `movl $0, addr + 4` 把高 32 位清零——这一点在 `long_mode.S` 的源码中已经处理了，但如果你是自己从头写的，很容易漏掉。

### 场景 3：BootInfo 字段值不对

在跳转之前（`jmp *%rax` 的前一条指令设断点），用 GDB 检查 0x7000 处的内容：

```
(gdb) x/4gx 0x7000
(gdb) x/6wx 0x7020
```

第一条命令查看 entry_point、kernel_phys_base、kernel_size、fb_addr。第二条查看 fb_width、fb_height、fb_pitch、fb_bpp 等。所有值应该合理——entry_point 应该是 0xFFFFFFFF80020000（小端序，实际内存中是 `00 00 02 80 FF FF FF FF`），fb_width 应该是你设置的 VESA 分辨率宽度（比如 1024 = 0x400）。

如果 fb_width 或 fb_height 的值看起来异常大（比如几万），很可能是在从 0x6400 读取 16 位值时没有做零扩展——`movl` 读 16 位会把高 16 位也带进来，导致读到垃圾数据。检查你的代码是否使用了 `movzwq`（零扩展 16 位到 64 位）来读取 width 和 height。

### 用 QEMU Monitor 查看页表

QEMU 的 Monitor 模式下可以直接查看 CPU 的页表映射：

```
(qemu) info mem
```

这个命令会列出所有已映射的虚拟地址范围。你应该能看到两行：0x0000000000000000 到 0x0000000000200000 的身份映射，以及 0xFFFFFFFF80000000 到 0xFFFFFFFF80200000 的高半核映射。如果只看到身份映射那一行，说明 PML4[511] 或 PDPT[510] 没有正确设置。

## 设计决策

### 为什么 BootInfo 使用固定物理地址

BootInfo 放在固定物理地址 0x7000 而不是动态协商。固定地址在调试时有巨大的优势——你可以在 QEMU monitor 中直接 `xp /824bx 0x7000` 查看完整的 BootInfo 内容，不需要先找指针再间接寻址。而且 Cinux 的 bootloader 是自写的，bootloader 和内核之间不存在"多个 bootloader 实现"的兼容性问题。如果是 GRUB 加载内核，那就得用 Multiboot 规范的动态地址方案——bootloader 把信息结构体的物理地址放在 `%ebx` 中传递，内核根据 `%ebx` 的值动态找到结构体。

### 为什么高半核映射复用 PD 条目

高半核映射复用了 identity mapping 的 PD 条目，而不是分配独立的页表。原因是 bootloader 阶段分配的固定页表内存（0x1000~0x3FFF）已经捉襟见肘——总共只有 3 个 4KB 页，分别给了 PML4、PDPT 和 PD。如果要建立独立的 PDPT 和 PD，还需要额外的 8KB 物理内存。更重要的是，bootloader 的页表只是临时的，内核启动后会建立自己的正式页表。启动阶段的简洁性比安全性更重要。等到内核的物理内存管理器上线后，可以在内核初始化阶段分配新的页表页面，把高半核映射独立出来，然后取消 identity mapping。

## Tag 边界说明

在继续动手之前，确认一下 004C 和 004A/004B 的分工边界。004A 负责的是 Real Mode 下的两件事——E820 内存探测（结果存在 0x5000）和从磁盘把 Mini Kernel 读到 0x20000。004B 负责的是 BootInfo 结构定义、磁盘读取从 4KB 升级到 416KB（分块循环）、以及 flat binary 构建系统。004C 的上篇（本篇）负责 Long Mode 阶段的三件事——高半核页表映射、BootInfo 填充、跳转到内核。004C 的下篇负责内核侧——SSE 启用、栈设置、BSS 清除、全局构造函数调用、C++ 特性验证。这个分工确保了每个 tag 只关注一个清晰的阶段：004A 是 Real Mode 数据准备，004B 是结构定义和磁盘加载，004C 是 Long Mode 交接和内核启动。

## 本章小结

| 概念 | 要点 |
|------|------|
| BootInfo 结构 | 824 字节 packed 结构，固定存放于物理 0x7000，Bootloader 和内核共用定义 |
| BootInfo 字段顺序 | entry_point(8B) + phys_base(8B) + size(8B) + fb_addr(8B) + fb_width(4B) + fb_height(4B) + fb_pitch(4B) + fb_bpp(4B) + mmap_count(4B) + _pad(4B) + mmap[32](768B) |
| C/C++ 兼容 static_assert | `#if defined(__cplusplus)` 分支选择 `static_assert` 和 `_Static_assert` |
| 高半核地址 | 0xFFFFFFFF80020000 = 0xFFFFFFFF80000000 + 0x20000，PML4[511] -> PDPT[510] -> PD[0] |
| 双重映射 | 同一 PD[0] 条目（0~2MB）同时服务身份映射和高半核映射，零额外物理页开销 |
| System V AMD64 ABI 调用约定 | 第一个参数通过 `%rdi` 传递，BootInfo 地址 = 0x7000 |
| 间接跳转 | AT&T 语法 `jmp *%rax`，星号不能省略 |
| 本篇验证字符 | `OPLJ`——`J` 表示 Bootloader 成功跳转到 Mini Kernel |
| debugcon 全局启用 | 将 `-debugcon file:debug.log` 从 DEBUG_FLAGS 移到 COMMON_FLAGS，确保 `make run` 也能看到 debugcon 输出 |
| 004C 边界 | 004A=E820+磁盘读取(Real Mode), 004B=BootInfo定义+416KB加载, 004C=高半核映射+填充+跳转(Long Mode)+内核启动 |
| 高半核 PD 索引 | 实际映射 0x20000 的是 PD[0]（0~2MB），而非 PD[1]——因为 0x20000 在 0~2MB 范围内 |
| Canonical Address | 0xFFFFFFFF80020000 的 bit 47=1，bits 63:48 全 1，符合 canonical 约束 |
| debugcon 端口 | I/O 端口 0xE9，QEMU isa-debugcon 设备，`-global isa-debugcon.iobase=0xe9` |
