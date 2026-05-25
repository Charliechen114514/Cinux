---
title: 004-boot-load-mini-kernel-a-1 · 内核加载 (A)
---

# 004 加载小内核（准备篇） -- Real Mode 的最后两件事

> 本章完成后的可见效果：QEMU debugcon 输出 `OPLJ` 字符序列（O=磁盘读取完成, P=保护模式, L=Long Mode, J=准备跳转小内核）
>
> 前置要求：已完成 `003_boot_long_mode`，理解 Long Mode 切换流程和四级页表

---

## 导语

上一章我们费了九牛二虎之力从保护模式切到了 Long Mode，在 debugcon 里打出了一个漂亮的 `L` 字符。但说实话，光进 Long Mode 还不算完——CPU 是跑起来了，可我们连一行内核代码都没有加载。现在的状况就好比你把房子盖好了、电也通了，但家具还没搬进来，住不了人。这一章我们要做的是：在 Real Mode 还没离开之前，把两件依赖 BIOS 中断的关键事情办了——探测系统内存布局（E820）和从磁盘把 Mini Kernel 的完整 flat binary 读进内存（INT 13h）。完成之后，Bootloader 就可以在后续 tag（004C）中填充 BootInfo 结构、做高半核映射、最终跳转到 Mini Kernel 执行。

## 概念精讲

### E820 内存探测：给物理内存画一张地图

你可能会问：为什么我们需要知道内存布局？内存不就是"有多少用多少"吗？在应用程序的世界里确实是这样——操作系统把内存管理得妥妥帖帖，`malloc` 直接用就好。但在写操作系统内核的时候，没有人帮你管内存了，你必须自己知道哪些物理地址是可用的 RAM、哪些是硬件保留的、哪些是 BIOS 数据区。如果你不小心把内核的数据写到了硬件保留区，轻则数据丢失，重则硬件直接不响应，最惨的是那种"有时候能用有时候不行"的随机崩溃——因为保留区里有些地址是 memory-mapped I/O，写进去的行为取决于硬件当前状态，完全不可预测。

BIOS 提供了好几种内存探测方法，但 E820（INT 0x15, AX=0xE820）是其中最强大、信息最完整的，也是现代操作系统通用的选择。你可以把它理解为一次"迭代式地图测绘"——每次调用返回一条记录（描述一段连续内存区域的基地址、长度和类型），通过一个续接值（EBX）告诉你"下一次从哪里继续"，直到续接值变为 0 表示枚举结束。

每条记录的 type 字段告诉你这段内存的性质。对我们来说最关键的是 type=1（可用 RAM）和 type=2（保留区，绝对不能碰）。QEMU 默认会返回 5 到 7 条记录，其中只有 type=1 的条目是真正可用的。这里有一个非常隐蔽的坑：E820 返回了保留区并不意味着出错了——保留区的存在是正常的，它可能被 BIOS、ACPI 表、或者 memory-mapped I/O 占用。如果你不过滤 type 字段，把所有返回的内存区域都当成可用 RAM 来用，那么恭喜你，你会收获一个看起来能跑但随时可能随机崩溃的系统。

关于 E820 的完整定义，可以参考 OSDev Wiki 的 [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) 页面。几个必须注意的细节：EAX 的完整值必须是 0x0000E820，高 16 位不能省略。ECX 应该设为 24 而不是 20——20 字节是旧版 BIOS 返回的结构大小，24 字节是 ACPI 3.0 扩展后的版本（多了一个 ACPI 属性字段）。

### INT 13h 扩展读取：用 LBA 告别 CHS 的噩梦

磁盘读取是 Bootloader 的另一个核心任务。早期的磁盘读取使用 CHS（Cylinder-Head-Sector）寻址模式——你需要告诉 BIOS 磁盘的哪个柱面、哪个磁头、哪个扇区。这种方式在软盘时代还凑合，但到了硬盘时代就彻底力不从了：CHS 寻址上限只有大约 8GB。INT 13h AH=0x42（扩展读取）用 LBA（Logical Block Addressing）彻底解决了这个问题——磁盘被看作一个从 0 开始编号的扇区序列（每个扇区 512 字节），你想读第几个扇区就直说，不用关心物理几何结构。

扩展读取使用一个叫 DAP（Disk Address Packet）的数据结构来描述读取请求，总共 16 字节。调用时 DL 寄存器必须设为 0x80（第一块硬盘的 BIOS 设备号），AH 设为 0x42（扩展读取功能号），DS:SI 指向 DAP 结构——注意这里用的是 DS:SI 而不是 ES:DI，别和 E820 的 ES:DI 搞混了。成功返回时 CF=0 且 AH=0。

这里有一个非常阴险的坑：BIOS INT 13h AH=42h 对单次读取的扇区数有上限，通常是 127 个扇区。我们的 Mini Kernel 有 416KB（832 个扇区），无法一次性读完，所以必须采用分块循环读取的方式——每轮最多读 127 个扇区，读完一批后更新 DAP 里的 LBA 和 buffer 地址，再读下一批。这个循环逻辑本身不复杂，但有一个特别阴险的陷阱：BIOS 调用会破坏 BX 和 BP 寄存器的值，所以每次 BIOS 调用前必须 pushw 保存，调用后 popw 恢复。

### 内存布局与栈冲突：为什么加载地址是 0x20000 而不是 0x10000

Mini Kernel 的加载地址选择不是随意的。如果从 0x10000 开始加载 416KB，磁盘写入的数据会从 0x10000 一路写到 0x71000——而 Stage2 的 Real Mode 栈设置是 SS=0x0900、SP=0xFFFE，对应的物理地址范围是 0x9000 到 0x19000。0x10000 到 0x19000 这段区域正好覆盖了栈空间！磁盘读取把数据写到了栈所在的区域，覆盖了函数返回地址，`ret` 的时候 CPU 跳到一个莫名其妙的地址，直接 triple fault。

解决方案是把加载地址设为 0x20000。0x20000 大于栈顶物理地址（0x19000），留了 28KB 的安全间隙。内核最大 416KB 从 0x20000 加到 0x88000，而 Protected Mode 的栈在 0x90000，中间还有 32KB 的安全距离。

### Real Mode 段地址计算：一个折磨了无数人的细节

在 Real Mode 下，CPU 访问内存用的是"段地址:偏移"的表示法，物理地址的计算公式是 `segment << 4 + offset`。比如你想访问物理地址 0x5000，段地址应该是 0x0500（0x0500 << 4 = 0x5000），偏移是 0x0000。如果你手滑把段地址写成了 0x5000，那实际访问的物理地址就变成了 0x50000，差了十倍——而且这种错误编译器不会报、链接器不会报、运行时也不一定立刻崩溃。

### 为什么必须在 Real Mode 做

既然我们最终要在 Long Mode 里运行内核，为什么 E820 和磁盘读取不能留到 Long Mode 再做？答案很简单——因为 BIOS 中断只在 Real Mode 下可用。一旦我们切换到保护模式，INT 0x15 和 INT 0x13 就彻底失效了——保护模式下没有中断向量表来路由这些软中断。所以在实模式阶段把所有依赖 BIOS 的事情一次性做完，是最简单也最可靠的方案。

## 动手实现

### Step 1: 创建公共引导模块

**目标**：创建 `boot/common/boot.S` 文件，作为 Real Mode 下内存探测和磁盘读取的公共模块。

**设计思路**：上一章我们把 Long Mode 相关代码独立成了 `boot/common/long_mode.S`，同样的思路在这里也适用。E820 和 INT 13h 的功能是通用的——不管以后 Bootloader 怎么演进，只要还需要通过 BIOS 获取信息，这两个函数就会一直存在。把它们从 stage2.S 中剥离出来，一方面避免 stage2.S 无限膨胀，另一方面也让代码职责更清晰：stage2.S 负责流程编排（先做什么后做什么），boot.S 负责具体的硬件操作。这个文件用 `.code16` 标记，因为所有操作都在 Real Mode 下进行。

**实现约束**：文件需要声明两个 `.global` 函数——`query_memory_map` 和 `load_kernel_from_disk`。错误处理标号 `disk_read_failed` 和 `disk_too_large` 作为 panic 跳转目标。错误消息字符串（E820 失败、磁盘读取失败、内核过大）放在 `.rodata` 段。文件头部需要用 `.extern` 声明外部依赖的函数和变量（panic 错误处理函数等）。

**验证**：文件创建后，确认路径正确即可，构建验证在后续步骤完成。

### Step 2: 定义 E820 相关常量

**目标**：在 `boot/common/boot.S` 开头用 `.set` 伪指令定义 E820 所有的魔数和地址常量。

**设计思路**：GNU AS 不能直接 `#include` C 头文件，所以常量必须用 `.set` 在汇编文件里重新定义。这些常量需要和 `boot/boot_info.h` 中的定义保持一致——如果两边对不上，内核读到的数据就会错位，后续内存管理全部乱套。常量分为四组：缓冲区地址（0x5000 区域的段地址和偏移分解）、MemoryMapEntry 结构的字段偏移（base=0、length=8、type=16、size=24）、BIOS 调用魔数（E820 签名 'SMAP' = 0x534D4150、功能号 0x0000E820），以及配置限制（最多 32 条记录）。

**实现约束**：GNU AS 的 `.set` 语法是 `.set SYMBOL, VALUE`。E820 的缓冲区我们固定放在物理地址 0x5000——这个地址是安全区域（BIOS 数据区之上、Bootloader 代码之下）。前 4 字节存放条目计数（count），接下来从偏移 4 字节开始存放条目数组，每条 24 字节，最多 32 条。段地址 0x0500（不是 0x5000！）对应物理地址 0x5000。

**踩坑预警**：E820_CMD 的值必须写成 0x0000E820，不能偷懒只写 0xE820。为了保险和可读性，始终写完整的 32 位值。另一个坑是 ECX 的值：必须传 24（ACPI 3.0 扩展大小），不是 20（旧版大小）。

**验证**：常量定义不需要单独构建验证，但可以和后续步骤一起验证。

### Step 3: 实现 query_memory_map 函数

**目标**：实现 E820 内存枚举函数，将结果保存到 0x5000 缓冲区。

**设计思路**：函数的整体结构是一个循环——初始化续接值 EBX=0，然后把 ES:DI 指向缓冲区（跳过前 4 字节的 count 字段），每次循环调用 INT 0x15，成功后 DI 前进 24 字节指向下一条目的位置，count 加 1，检查 EBX 是否归零决定是否继续。循环退出后，count 的值拷贝到 CX 作为返回值。函数开头用 `pushaw` 保存所有 16 位通用寄存器以及 `pushw %es` 和 `pushw %ds`，因为我们会修改 ES（指向缓冲区）和 DS（指向 count 字段）。退出时对称恢复。

在循环体内，每次调用前需要重新加载 EDX='SMAP' 和 EAX=0x0000E820——因为 BIOS 可能会破坏这些寄存器的值。ECX=24 也需要每次重设。调用后检查三个条件：CF=0（没有进位）、EAX='SMAP'（BIOS 签名确认）、ECX>=20（BIOS 至少返回了 20 字节的数据）。三个条件全部满足才算成功，任何一个不满足就跳转到错误处理。

**实现约束**：ES 需要设为 E820 条目数组的段地址（0x0500），DI 初始值为 0x0004（跳过前 4 字节的 count）。DS 也需要设为 0x0500。条目数组从偏移 0x0004 开始。循环计数用物理地址 0x5000 处的 32 位 count 字段——这样设计是为了让内存探测结果即使在函数返回后也持久保存在固定地址，后续内核启动时可以直接从 0x5000 读取。最大条目数限制为 32，超过后强制退出，防止缓冲区溢出。

**踩坑预警**：最大的坑是段寄存器的设置。要访问物理地址 0x5000，段地址是 0x0500——但如果你手滑写成了 0x5000，CPU 会去访问物理地址 0x50000。另一个容易犯的错误是 DI 的递增量——每条记录 24 字节，所以 DI 每次要加 24（E820_ENTRY_SIZE），不是 20。

**验证**：在 GDB 中可以在 `query_memory_map` 返回后检查 0x5000 处的数据。`x/4bx 0x5000` 查看 count（应该是 QEMU 返回的条目数，通常 5-7），`x/24bx 0x5004` 查看第一条记录的 base、length、type 字段。

### Step 4: 定义磁盘读取常量和 DAP 结构

**目标**：定义 INT 13h 扩展读取所需的所有常量，包括 DAP 结构的字段偏移、磁盘布局参数和加载地址。

**设计思路**：磁盘读取的常量包括 DAP 结构的 6 个字段偏移量（size=0、reserved=1、count=2、buffer_offset=4、buffer_segment=6、lba=8）、DAP 在低内存中的固定位置（0x7B00），以及磁盘布局参数。Mini Kernel 的起始 LBA 是 16（前面 1 个扇区给 MBR、15 个扇区给 Stage2），总共 832 个扇区（416KB）。加载到物理地址 0x20000，对应段地址 0x2000、偏移 0x0000。BIOS 单次读取上限是 127 个扇区（DISK_MAX_SECTORS_PER_CALL），所以需要分块循环读取。

**实现约束**：DAP 结构放在物理地址 0x7B00，段地址 0x07B0。Mini Kernel 的起始 LBA 是 16。加载到物理地址 0x20000，对应段地址 0x2000、偏移 0。MINI_KERNEL_SECTORS = 832。DAP 的 size 字段必须为 16，reserved 必须为 0。注意我们还需要定义 MINI_KERNEL_LOAD_SEG = 0x2000 和 MINI_KERNEL_LOAD_OFF = 0x0000 用于计算每轮的 buffer 地址。

**踩坑预警**：DAP 结构里的 LBA 字段是 64 位的，高 32 位必须显式清零——不能假定内存里是干净的。

**验证**：常量定义和后续步骤一起验证。

### Step 5: 实现 load_kernel_from_disk 函数

**目标**：实现磁盘读取函数，使用分块循环将 Mini Kernel 的完整 flat binary（832 扇区 = 416KB）从磁盘 LBA 16 读取到物理内存 0x20000。

**设计思路**：函数的结构是一个循环读取——用 BX 寄存器跟踪已读取的扇区总数，每轮循环先计算"还剩多少扇区"（832 - BX），取"剩余扇区"和 127 的较小值作为本轮读取量。然后构建 DAP 结构（设置 size=16、count=本轮读取量、buffer 地址需要根据 BX 动态计算、LBA=16+BX 且高 32 位清零），调用 INT 0x13 AH=0x42，检查 CF 和 AH 判断成功与否。函数用 `pusha`/`popa` 保存/恢复 16 位寄存器，以及单独保存 ES 和 DS。

buffer 地址的动态计算是这样的：基础段地址 0x2000，加上 BX * 32（每个扇区 512 字节，512 / 16 = 32 个"段单位"）。所以每轮的 buffer_segment = MINI_KERNEL_LOAD_SEG + (BX << 5)，buffer_offset 始终为 0。LBA 的计算是 MINI_KERNEL_LBA + BX。

这种分块循环设计是必须的——BIOS 单次读取不能超过 127 个扇区，832 个扇区至少需要 7 轮读取。每轮读完一轮后 BX 加上本轮读取的扇区数，更新 DAP 参数，继续下一轮，直到 BX >= 832 时退出循环。

**实现约束**：DAP 构建时先设 ES 指向 0x07B0，DI 指向 0x0000，然后逐字段写入。INT 13h AH=0x42 要求 DS:SI 指向 DAP（不是 ES:DI），DL=0x80 表示第一块硬盘。成功标志是 CF=0 且 AH=0。每轮循环结束后 BX 加上本轮读取量（BP），然后跳回循环开头。整个循环结束后恢复寄存器，输出 'O' 到 debugcon（表示磁盘读取成功），AX 返回总读取扇区数（832）。

**踩坑预警**：有三个我们实打实踩过的坑。第一个是 DS:SI vs ES:DI 的混淆——INT 13h AH=0x42 要求 DAP 通过 DS:SI 传递，不是 ES:DI。第二个是 DL 必须设为 0x80——如果 DL 里恰好还是之前 E820 调用留下的残留值，BIOS 返回错误。第三个是 BIOS 调用会破坏 BX 和 BP 寄存器——所以每次 BIOS 调用前必须 pushw 保存 BX 和 BP，调用后 popw 恢复，否则循环计数全部乱套。LBA 的高 32 位也必须显式清零。

**验证**：在 GDB 中可以在函数返回后检查 0x20000 处的数据。Mini Kernel 的 flat binary 应该出现在这里。检查 debugcon 输出应该看到 'O' 字符。

### Step 6: 在 stage2.S 中集成调用

**目标**：修改 `boot/stage2.S`，在 VESA 初始化完成后、进保护模式之前调用 `query_memory_map` 和 `load_kernel_from_disk`。

**设计思路**：stage2.S 是整个 Bootloader 的流程编排文件——它按照固定顺序调用各个功能模块。在 tag 003 中，stage2 的调用链是：打印消息 -> 开启 A20 -> VESA 初始化 -> 进保护模式 -> 页表设置 -> 进 Long Mode。现在我们需要在"VESA 初始化完成"和"进保护模式"之间插入两个新的调用：先 `query_memory_map` 再 `load_kernel_from_disk`。这个顺序是有讲究的——E820 不依赖磁盘状态，可以先做；磁盘读取也不依赖 E820 结果，但按"先探测后加载"的逻辑顺序排列更清晰。两个函数都必须在 CLI（关中断）之前调用，因为 INT 0x15 和 INT 0x13 都是 BIOS 中断，需要中断处于开启状态。

**实现约束**：在 stage2.S 中用 `.extern` 声明 `query_memory_map` 和 `load_kernel_from_disk`。调用点在 `vesa_save_framebuffer_info` 之后、CLI 之前。两个函数之间不需要额外的寄存器保存——因为 `query_memory_map` 内部已经做了完整的寄存器保存/恢复。调用完成后 stage2 继续原有的保护模式切换流程。

**踩坑预警**：调用时机千万别搞错——必须在 VESA 之后（因为 VESA 还在用 BIOS 中断）且在 CLI 之前（因为后续的 BIOS 调用需要中断开启）。

**验证**：用 `objdump -d build/boot/stage2` 反汇编，确认在 VESA 调用和 CLI 之间能看到 `call query_memory_map` 和 `call load_kernel_from_disk`。

### Step 7: 创建 Mini Kernel 占位

**目标**：创建 `kernel/mini/` 目录下的小内核源文件，作为后续 Bootloader 跳转的目标。

**设计思路**：Mini Kernel 在当前 tag（004A）阶段只需要做一个最简单的事情——证明它能被编译和写入磁盘镜像。所以我们先写一个最精简的入口点。现阶段它的唯一意义是让构建系统有一个可编译、可链接、可写入磁盘镜像的 ELF 文件——后续 tag 会让它真正跑起来，加入 SSE 启用、栈设置、BSS 清零、C++ 运行时初始化等。

链接脚本 `kernel/mini/linker.ld` 定义了内存布局：物理加载地址（LMA）是 0x20000（128KB），虚拟基址是 0xFFFFFFFF80000000（高半核），所以 VMA = 0xFFFFFFFF80020000。入口点 `_start` 定义在 `kernel/mini/arch/x86_64/boot.S` 中。

**实现约束**：Mini Kernel 的 C++ 代码使用 `-ffreestanding -fno-exceptions -fno-rtti -mcmodel=large -mno-red-zone` 编译选项。`-mcmodel=large` 是必须的——小内核的虚拟地址在 0xFFFFFFFF80000000 附近，超出了默认模型的范围。`-mno-red-zone` 是内核代码的标配。链接完成后用 `objcopy -O binary` 转换成 flat binary，Bootloader 直接读取这个 flat binary 到内存即可，无需解析 ELF header。

**踩坑预警**：`-mcmodel=large` 是必须的，不能用默认的 `-mcmodel=small`。

**验证**：确认 `kernel/mini/` 目录下有 `main.cpp`、`linker.ld`、`arch/x86_64/boot.S` 和 `CMakeLists.txt`。

### Step 8: 更新构建系统

**目标**：更新 CMake 构建系统和镜像构建脚本，使 Mini Kernel 被编译并打包到磁盘镜像中。

**设计思路**：构建系统的改动分三层。第一层是 Mini Kernel 自身的构建：`kernel/mini/CMakeLists.txt` 定义了 `mini_kernel` 目标，编译 Mini Kernel 的所有源文件，链接生成 ELF 可执行文件，然后用 `objcopy -O binary` 把 ELF 转换成纯二进制文件（`mini_kernel.bin`）。Bootloader 直接跳到入口点执行，flat binary 模式下不需要 ELF 加载器。

第二层是磁盘镜像构建：`scripts/build_image.sh` 脚本需要知道 Mini Kernel 的二进制文件路径和磁盘布局。磁盘布局是——Sector 0 给 MBR（512 字节），Sector 1-15 给 Stage2（最多 15 个扇区 = 7680 字节），Sector 16 开始给 Mini Kernel。脚本先用 `dd` 创建一个全零的空白镜像，然后依次写入 MBR、Stage2 和 Mini Kernel。写入 Mini Kernel 时用 `bs=512 seek=16` 参数，意思是"跳到第 16 个扇区开始写入"。

第三层是 CMake 的 `cmake/qemu.cmake`：`run` 目标需要依赖 `mini_kernel`——确保每次 `make run` 之前 Mini Kernel 都会被重新编译。`build_image.sh` 的调用参数中需要加入 Mini Kernel flat binary 文件的路径。

**验证**：执行完整构建后，确认 `build/kernel/mini/mini_kernel.bin` 文件存在且大小合理（应该有几百 KB）。确认 `build/cinux.img` 文件存在且大于 416KB（因为包含了 Mini Kernel 的数据）。

### Step 9: 验证完整启动链

**目标**：在 Long Mode 入口中确认 debugcon 输出 `OPLJ` 序列，验证整个启动链条到磁盘读取和 BootInfo 跳转全部正确。

**设计思路**：这是整个 tag 004 系列的最后验证。在 `boot/common/boot.S` 的 `load_kernel_from_disk` 成功路径中输出 'O' 字符。在 stage2.S 的保护模式入口输出 'P'，Long Mode 入口输出 'L'，然后 004C 在跳转前输出 'J'。完整的字符序列是 `OPLJ`。

**验证**：这是最终验证步骤。完整构建并运行后，debugcon 日志应该包含以下字符序列：`O`（磁盘读取完成）-> `P`（保护模式切换成功）-> `L`（Long Mode 切换成功）-> `J`（BootInfo 填充完成，准备跳转）。如果缺少某个字符，说明对应的步骤出了问题。

## 构建与运行

在项目根目录执行完整构建。确保 CMake 配置正确（Debug 模式），然后构建并运行：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
make run
```

QEMU 启动后，串口终端会输出 Bootloader 的启动信息（`Stage2 OK`、`Mode info OK, switching...`）。按 `Ctrl+C` 退出 QEMU（或者等内核进入停机循环），然后检查 debugcon 日志：

```
cat debug.log
```

预期看到完整的字符序列：`OPLJ`。其中每个字符的含义是：`O` = 磁盘读取完成、`P` = 保护模式、`L` = Long Mode、`J` = BootInfo 填充完成（准备跳转）。如果缺少某个字符，说明对应的步骤出了问题。

## 调试技巧

### E820 失败排查

如果 E820 调用失败（函数跳转到 `panic`），最可能的原因有三个。第一，段地址设错了——用 GDB 检查调用前 DS 和 ES 的值，应该是 0x0500 而不是 0x5000。第二，EAX 的值不是 0x0000E820——检查常量定义是否正确。第三，ECX 不是 24——某些 BIOS 对这个值很敏感。

```
(gdb) break query_memory_map
(gdb) info registers eax ebx ecx edx es ds
```

### 磁盘读取失败排查

如果磁盘读取失败（debugcon 没有出现 'O'），检查几个方面。首先确认 DL=0x80——这是最常见的遗漏。然后用 GDB 检查 DAP 的内容是否正确：`x/16bx 0x7B00`。确认 BX 和 BP 是否被正确保存和恢复——如果 BIOS 调用破坏了 BX，循环会读取错误的扇区。确认 Mini Kernel 确实被写入了磁盘镜像——用 `xxd build/cinux.img | head -n 100` 查看镜像内容，在 LBA 16（偏移 0x2000）的位置应该能看到 flat binary 数据。

### 用 GDB 跟踪完整启动链

完整的调试流程是在一个终端启动 QEMU 调试模式，在另一个终端连接 GDB：

```
# 终端 1
make run-debug

# 终端 2
gdb -ex "target remote :1234" build/boot/stage2
(gdb) break query_memory_map
(gdb) break load_kernel_from_disk
(gdb) continue
```

在 `query_memory_map` 返回后检查 E820 数据：

```
(gdb) x/4bx 0x5000
(gdb) x/6gx 0x5004
```

在 `load_kernel_from_disk` 返回后检查加载的内核数据：

```
(gdb) x/16bx 0x20000
```

## 本章小结

| 概念 | 要点 |
|------|------|
| E820 内存探测 | INT 0x15 AX=0x0000E820，迭代调用，EBX 续接，结果存 0x5000 |
| E820 条目格式 | base(8B) + length(8B) + type(4B) + acpi(4B) = 24 字节/条 |
| E820 type 字段 | 1=可用 RAM，2=保留区，必须过滤 type 后再使用 |
| INT 13h 扩展读取 | AH=0x42，DAP 通过 DS:SI 传递（不是 ES:DI） |
| DAP 结构 | 16 字节：size + reserved + count + buffer_seg:off + LBA(64bit) |
| 分块循环读取 | 每轮最多 127 扇区，BX 跟踪已读扇区，BP 保存本轮读取量 |
| 加载地址选择 | 0x20000（避开 Real Mode 栈 0x9000~0x19000 和 Protected Mode 栈 0x90000） |
| Mini Kernel flat binary | 832 扇区（416KB），LBA 16 开始，加载到 0x20000 |
| 段地址计算 | 物理地址 = segment << 4 + offset，0x5000 -> seg=0x0500 |
| 验证字符序列 | O -> P -> L -> J |
