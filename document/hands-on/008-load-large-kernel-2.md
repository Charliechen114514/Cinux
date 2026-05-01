# 008-2 Hands-on: ELF64 解析器与大内核加载器

## 导语

上一节我们把 ATA PIO 磁盘驱动搞定了，mini kernel 现在能从磁盘读取任意扇区的数据。但光能读数据还不够——我们需要知道读出来的数据是什么格式、该怎么处理、最终要放到内存的什么位置。这就是 ELF 解析器要干的事情。

ELF（Executable and Linkable Format）是 Linux/Unix 系统的标准可执行文件格式，也是我们"大内核"在磁盘上的存储格式。大内核以 ELF64 形式编译，mini kernel 需要解析它的头部信息、找到所有需要加载的段（PT_LOAD）、把每个段搬到正确的物理地址、清零 BSS 区域，最后跳转到入口点执行。整个流程由大内核加载器编排——它先通过 ATA 驱动把整个 ELF 文件读到一个 staging buffer，然后调用 ELF 解析器完成加载。

完成本章后，我们会看到 mini kernel 成功从磁盘读取 mini kernel 自身的头部并验证 ELF magic，证明整个读取+解析管线已经就位。虽然真正的大内核还不存在（那是后续 milestone 的事），但加载器代码已经完全写好了。

本章的前置知识是上一节的 ATA PIO 驱动（用于读磁盘）和上一章（007）的 IDT/中断处理（用于错误报告）。

---

## 概念精讲

### ELF 格式为什么是加载大内核的关键？

我们在之前的章节里，内核是被 Bootloader 以"裸二进制"（flat binary）的方式加载的——`objcopy -O binary` 把 ELF 文件里的所有内容按地址平铺成一个连续的二进制块，没有段信息，没有入口点信息。Bootloader 只能盲目地把它放到固定地址然后跳转。这种方式简单粗暴，但缺点也很明显：内核必须链接到一个固定的物理地址，而且所有段（代码、数据、BSS）必须连续排列。

ELF 格式就灵活得多。ELF 文件里有程序头表（Program Header Table），每个条目描述一个"段"（segment）的加载信息——它在文件中的偏移（p_offset）、要加载到的虚拟地址（p_vaddr）和物理地址（p_paddr）、在文件中占多少字节（p_filesz）、在内存中占多少字节（p_memsz）。特别是 p_memsz 可以大于 p_filesz，多出来的部分就是 BSS——需要清零但不需要从文件读取。这些信息让加载器能够精确地把每个段放到正确的位置。

对我们的 higher-half 内核来说，ELF 格式尤其重要。内核代码链接在 0xFFFFFFFF80000000 以上的虚拟地址空间，但 mini kernel 运行在物理地址身份映射模式下，需要把内核加载到对应的物理地址。ELF 头里的入口点（e_entry）是虚拟地址，加载器需要减去 0xFFFFFFFF80000000 才能得到物理入口——这就是后面要讲的 higher-half 地址转换。

```
ELF 加载过程（简化版）：

磁盘上的 ELF 文件:
+----------------+
|  ELF Header    |  e_entry = 0xFFFFFFFF80001000 (virtual)
|  (64 bytes)    |  e_phoff -> Program Header Table
+----------------+
|  Phdr[0]       |  PT_LOAD: p_paddr=0x100000, p_filesz=0x2000, p_memsz=0x3000
|  Phdr[1]       |  PT_LOAD: p_paddr=0x200000, p_filesz=0x1000, p_memsz=0x1000
+----------------+
|  Segment 0     |  代码段数据 (0x2000 bytes)
|  Segment 1     |  数据段数据 (0x1000 bytes)
+----------------+

加载到内存后:
物理地址 0x100000:  [代码段 0x2000 字节][BSS 清零 0x1000 字节]
物理地址 0x200000:  [数据段 0x1000 字节]

入口点物理地址 = 0xFFFFFFFF80001000 - 0xFFFFFFFF80000000 = 0x1000
```

### ELF64 的关键结构

ELF64 文件的开头是 ELF header（64 字节），其中最重要的字段包括：e_ident（前 4 字节是 magic number `\x7FELF`，用于验证文件格式；第 5 字节标识 32/64 位；第 6 字节标识字节序）、e_type（文件类型，2 表示可执行文件）、e_machine（目标架构，0x3E 即 62 表示 x86-64）、e_entry（入口点虚拟地址）、e_phoff（程序头表在文件中的偏移）、e_phnum（程序头的数量）、e_phentsize（每个程序头的大小，通常是 56 字节）。

程序头（Program Header，56 字节）描述一个段的信息。我们只关心 p_type 为 1（PT_LOAD）的段。PT_LOAD 段有 p_offset（段数据在文件中的起始偏移）、p_vaddr（目标虚拟地址）、p_paddr（目标物理地址）、p_filesz（文件中的大小）、p_memsz（内存中的大小，大于 p_filesz 的部分是 BSS）、p_flags（权限标志：可读 PF_R=4、可写 PF_W=2、可执行 PF_X=1）。

### Higher-Half 内核的地址转换

Cinux 的大内核采用 higher-half 设计——内核代码链接在高半部分地址空间（0xFFFFFFFF80000000 以上），这样用户空间的低 3GB（或更多）地址空间完全留给用户进程。但 mini kernel 运行时只有物理地址的身份映射，没有建立内核的虚拟地址映射。

所以 ELF header 里的 e_entry 是一个虚拟地址（比如 0xFFFFFFFF80001000），而我们跳转时需要用物理地址。转换方法很简单：如果入口点地址大于等于 0xFFFFFFFF80000000，就减去这个基址得到物理地址；否则说明入口点本来就在低地址，直接使用即可。同理，段的目标地址使用 p_paddr（物理地址）而不是 p_vaddr（虚拟地址），因为在 mini kernel 的身份映射环境下，物理地址就是可以直接访问的地址。

### Staging Buffer 与 Bounds Checking

大内核加载器的设计是先通过 ATA 驱动把整个 ELF 文件读到一个 staging buffer（物理地址 0x1000000，即 16MB），然后再由 ELF 解析器从 staging buffer 中解析段并拷贝到目标地址。这个设计的一个重要考量是：我们事先不知道 ELF 文件有多大，所以读取一个"保守的上限"——512 个扇区（256KB）。这个数字远大于我们预期的内核大小，但如果内核真的超出了这个限制呢？

这就是 staging_size 参数的作用。ELF 加载器收到 staging buffer 的大小信息后，会检查每个段的 `p_offset + p_filesz` 是否超出了 staging buffer 的范围。如果超出了，说明我们读的数据不够——ELF 文件比我们读的扇区数要大，段数据被截断了。这时候直接报错中止，而不是默默读到垃圾数据然后跳转到一个不知什么地方的入口点。

### 大内核加载器的流程编排

`load_big_kernel()` 是整个加载过程的编排者，它把 ATA 驱动和 ELF 解析器串联起来。流程是这样的：先用 ATA 的 `read()` 函数从指定的 LBA 扇区开始，读取固定数量的扇区到 staging buffer；然后检查 staging buffer 的前四个字节是不是 ELF magic；最后调用 ELF 解析器的 `load_elf()` 函数，传入 staging buffer 的地址和大小，得到入口点物理地址。

其中 ELF magic 的快速检查是一个保险措施——如果在 staging buffer 的开头没有看到 `\x7FELF`，说明磁盘上那个 LBA 位置根本不是 ELF 文件，可能是空的或者放错了位置。与其等到 ELF 解析器里面一步步验证失败后报一堆错，不如一开始就发现明显的问题。

---

## 动手实现

### Step 1: 实现 ELF64 解析器——头文件与结构体定义

**目标**：定义 ELF64 规范中的关键常量和数据结构，包括 ELF header（Elf64_Ehdr）和 Program Header（Elf64_Phdr）。

**设计思路**：ELF64 是一个有严格二进制布局的标准格式，结构体定义必须和文件中的字节布局完全对应。我们直接按照 ELF64 规范定义 Elf64_Ehdr（64 字节）和 Elf64_Phdr（56 字节），使用 `__attribute__((packed))` 防止编译器插入填充字节。常量方面需要定义 ELF magic（0x464C457F）、class 64-bit（2）、little-endian（1）、executable type（2）、x86-64 machine（62）、PT_LOAD（1）以及权限标志 PF_X/PF_W/PF_R。

**实现约束**：所有定义放在一个命名空间下。结构体字段类型必须严格匹配规范——offset、size、address 类字段用 `uint64_t`，type、flags 用 `uint32_t`，version 等小字段用对应的类型。e_ident 是 16 字节的数组。Elf64_Phdr 的字段顺序必须严格按照规范：p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align。注意 ELF64 中 p_flags 的位置（在 p_type 后面）和 ELF32 不同（在最后面）。

**踩坑预警**：ELF64 和 ELF32 的程序头结构不同——在 ELF64 里 p_flags 在第二个字段（offset 0x04），而在 ELF32 里 p_flags 在最后一个字段。如果你照着 ELF32 的资料来写 ELF64 的结构体，字段顺序就错了，解析出来的值全都是乱的。这个坑非常隐蔽，因为编译不会报错，但运行时读出来的 p_offset、p_vaddr 等全是垃圾值。

**验证**：编译通过，并且 `sizeof(Elf64_Ehdr)` 应该是 64，`sizeof(Elf64_Phdr)` 应该是 56。

---

### Step 2: 实现 ELF64 解析器——parse_elf_header()

**目标**：验证一个内存缓冲区包含合法的 ELF64 x86-64 可执行文件的头部。

**设计思路**：验证过程是一步步检查 ELF header 的各个字段。首先检查指针非空；然后验证 magic number（前四个字节是 0x7F, 'E', 'L', 'F'）；接着检查 class 是否为 64 位（e_ident[4] == 2）、字节序是否为 little-endian（e_ident[5] == 1）、目标架构是否为 x86-64（e_machine == 62）、文件类型是否为可执行（e_type == 2）；最后检查程序头表是否存在（e_phoff 和 e_phnum 都不为零）。每一步失败都通过 kprintf 输出详细的错误信息，告诉你具体是哪个字段不对。

**实现约束**：函数签名为 `bool parse_elf_header(const void* elf)`，接收指向 ELF 数据的指针，返回 true/false。函数不打印成功信息，只打印失败信息——因为调用者可能在探测多个位置，失败了不代表有问题，但成功了需要知道。

**踩坑预警**：magic number 的字节顺序很重要——第一个字节是 0x7F，然后依次是 'E'(0x45)、'L'(0x4C)、'F'(0x46)。如果你把它们当成一个 32 位整数来比较，需要考虑字节序。在小端系统上 0x464C457F 就是正确的 magic 值，但逐字节比较更直观也更不容易出错。

**验证**：在 main.cpp 中，读取 LBA 16（mini kernel 在磁盘上的位置）的一个扇区，调用 parse_elf_header 验证。由于 mini kernel 可能以 flat binary 格式存储（取决于 objcopy 步骤），验证结果可能成功也可能失败，但函数不应崩溃。

---

### Step 3: 实现 ELF64 解析器——calculate_kernel_size()

**目标**：计算 ELF 文件中所有 PT_LOAD 段在内存中占用的总大小。

**设计思路**：遍历所有程序头，找到类型为 PT_LOAD 的段，记录最低的 p_paddr 和最高的 p_paddr + p_memsz，两者的差值就是总占用大小。这个函数主要用于预检查——在真正加载之前，看看目标内存区域是否有足够的空间。

**实现约束**：函数签名为 `size_t calculate_kernel_size(const Elf64_Ehdr* ehdr)`。通过 ehdr 的 e_phoff 和 e_phentsize 定位程序头表，逐个遍历。如果没有任何 PT_LOAD 段，返回 0。需要处理 e_phnum 为 0 或 ehdr 为空的边界情况。

**踩坑预警**：计算"最高地址"时要用 `p_paddr + p_memsz` 而不是 `p_paddr + p_filesz`——BSS 部分虽然在文件里不存在，但在内存中确实占空间。如果只算 filesz，可能会低估内核的实际内存需求。

**验证**：这一步暂时没有独立的运行验证，它会在后续的 ELF 加载过程中被间接使用。

---

### Step 4: 实现 ELF64 解析器——load_elf()

**目标**：解析 ELF 文件并将所有 PT_LOAD 段加载到正确的物理地址，返回入口点。

**设计思路**：这是 ELF 解析器的核心函数。首先调用 `parse_elf_header` 验证 ELF 头；然后遍历所有程序头，对每个 PT_LOAD 段：先做 bounds checking（`p_offset + p_filesz` 不超过 staging_size），然后用 memcpy 把文件数据从 staging buffer 拷贝到 p_paddr 指定的物理地址，再用 memset 把 BSS 部分（p_memsz - p_filesz）清零。所有段加载完毕后，从 ELF header 读取 e_entry，做 higher-half 地址转换（如果 >= 0xFFFFFFFF80000000 就减去基址），返回物理入口地址。

**实现约束**：函数签名为 `uint64_t load_elf(void* elf_src, uint64_t staging_size)`。返回入口点物理地址，0 表示失败。段的目标地址直接使用 p_paddr（强制转换为指针），这在 mini kernel 的身份映射环境下是可行的。拷贝使用前面实现的 memcpy 和 memset 函数。每个段的加载进度都通过 kprintf 输出日志。

**踩坑预警**：bounds checking 非常重要——如果你不小心，段数据的偏移加上大小可能超出你实际从磁盘读取的字节数。这时 memcpy 会从 staging buffer 外面的内存读数据，读到什么全看运气。这种 bug 在小内核上不容易发现（因为 staging buffer 够大），但在内核变大后就会暴露。另外 higher-half 地址转换的方向要搞对——是把虚拟地址转换为物理地址（减去基址），不是反过来。

**验证**：目前没有真正的大内核可以加载，但可以通过读取 mini kernel 自身（如果是 ELF 格式存储的话）来部分验证解析逻辑。

---

### Step 5: 实现大内核加载器——头文件与常量

**目标**：定义大内核加载所需的常量——内存地址、磁盘 LBA、扇区数量。

**设计思路**：常量定义反映了整个系统的内存布局约定。mini kernel 被 Bootloader 加载到 0x20000（128KB 位置），大内核的 staging buffer 放在 0x1000000（16MB 位置）——这个地址远离 mini kernel 和 bootloader 结构，不会产生冲突。大内核在磁盘上的起始 LBA 是 848——前面 16 个扇区给 MBR + Stage2，然后 832 个扇区给 mini kernel（416KB）。大内核最多读 512 个扇区（256KB），这是一个保守的上限。

**实现约束**：所有常量定义为命名空间内的 `constexpr` 值。MINI_KERNEL_LOAD_ADDR = 0x20000, BIG_KERNEL_LOAD_ADDR = 0x1000000, BIG_KERNEL_LBA = 848, BIG_KERNEL_MAX_SECTORS = 512。`load_big_kernel(uint64_t disk_lba)` 是唯一的公开函数，返回入口点物理地址。

**踩坑预警**：BIG_KERNEL_LBA 的值必须和构建脚本中磁盘镜像的布局一致。如果构建脚本把大内核放到了不同的 LBA 位置，但加载器还用的是旧的 LBA 值，读出来的就是错误的数据。这种跨模块的常量约定很容易在重构时被遗漏。

**验证**：编译通过即可。

---

### Step 6: 实现大内核加载器——load_big_kernel()

**目标**：编排整个大内核加载流程——磁盘读取 + ELF 验证 + 段加载。

**设计思路**：这是整个 tag 的"集成"步骤，把前面实现的所有模块串起来。流程是：先用 ATA 驱动从磁盘读取固定数量的扇区到 staging buffer；然后检查 staging buffer 开头的 ELF magic 做快速验证；最后调用 ELF 解析器完成完整的解析和段加载。如果任何一步失败，返回 0 表示错误。

**实现约束**：函数签名为 `uint64_t load_big_kernel(uint64_t disk_lba)`。ATA 读取的目标地址就是 BIG_KERNEL_LOAD_ADDR（staging buffer），传给 load_elf 时也用同一个地址。staging_size 由 BIG_KERNEL_MAX_SECTORS × 512 计算得出。每个步骤都有详细的 kprintf 日志输出。

**踩坑预警**：ELF magic 的快速检查应该用逐字节比较而不是直接读取 uint32_t——因为对齐问题，在内存中的任意位置强制转换 uint32_t 可能导致未对齐访问，虽然 x86 上通常不会崩溃但不是好习惯。

**验证**：目前没有大内核可以加载，但可以在 main.cpp 中测试 ATA 读取和 ELF 验证。构建运行后，串口应显示：

```
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[DEMO] ELF header detected at disk LBA 16 (mini kernel)
```

或者如果 mini kernel 是 flat binary：

```
[DEMO] No valid ELF header at LBA 16 (expected for flat binary)
```

---

### Step 7: 更新 main.cpp 集成所有模块

**目标**：在 mini kernel 的主函数中初始化 ATA 驱动并运行 demo 测试。

**设计思路**：main.cpp 的启动流程更新为：初始化串口 → 初始化 GDT → 初始化 IDT → 初始化 PMM → 触发 #BP 测试中断 → 初始化 ATA → 读取 MBR 验证引导签名 → 读取 LBA 16 验证 ELF header。这些 demo 步骤验证了整个读取+解析管线的可用性。

**实现约束**：ATA 初始化失败时进入死循环（`while(1) __asm__ volatile("cli; hlt")`），因为磁盘驱动不可用意味着后续的内核加载无法进行。MBR 读取使用一个静态缓冲区 `g_sector_buf[512]`，16 字节对齐。

**踩坑预警**：ATA 初始化必须在 PMM 和 IDT 之后——PMM 提供内存管理能力（虽然当前 ATA 驱动不需要动态分配），IDT 提供异常处理能力（万一 ATA 操作触发了 #GP 或 #PF，有中断处理器来报告而不是 triple fault）。

**验证**：编译并运行，完整的串口输出应该类似：

```
Cinux Mini Kernel v0.1.0
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[MINI] Milestone 008 complete. Waiting for big kernel (009+)...
```

---

## 构建与运行

```bash
# 完整构建
cd build && cmake --build .

# 运行
qemu-system-x86_64 -hda disk.img -serial stdio -display none
```

如果你想调试 ELF 解析过程，可以在 `load_elf` 中多加一些 kprintf，观察每个段的地址和大小是否符合预期。

---

## 调试技巧

### 常见 Bug 1: "No ELF magic at staging buffer"

这说明 staging buffer 的开头不是 ELF 文件。可能原因：BIG_KERNEL_LBA 的值不对（磁盘上那个位置是空的或者放了别的数据），或者磁盘镜像构建时大内核根本没有被写入。排查方法：用 `xxd` 或 `hexdump` 检查磁盘镜像在对应 LBA 位置的内容。

### 常见 Bug 2: 段加载后数据不对——跳转到大内核入口后 triple fault

这说明段数据拷贝有问题。可能原因：p_paddr 的值不在可访问的物理内存范围内（比如超过了 PMM 管理的范围），或者 memcpy/memset 函数有 bug（比如方向搞反了）。排查方法：在 load_elf 中逐段打印目标地址和拷贝大小，用 QEMU monitor 的 `xp` 命令检查目标地址处的数据。

### 常见 Bug 3: BSS 没有被正确清零

这会导致全局变量和静态变量包含垃圾值。可能原因：memset 的参数传错了（count 用了 filesz 而不是 bss_size），或者 memset 函数本身有 bug。排查方法：在 BSS 清零后立即读取目标地址的数据验证是否为 0。

QEMU monitor 中检查内存内容的命令：
```
# 查看物理地址 0x100000 处的 64 字节
xp /64bx 0x100000

# 查看物理地址 0x1000000 处的内容（staging buffer 开头）
xp /16bx 0x1000000
```

---

## 本章小结

| 概念/结构 | 关键要点 |
|-----------|---------|
| ELF magic | `\x7F` + `ELF`（前 4 字节） |
| Elf64_Ehdr | 64 字节，包含入口点、程序头偏移和数量 |
| Elf64_Phdr | 56 字节，描述一个段的位置、大小、权限 |
| PT_LOAD | type=1，需要加载到内存的段 |
| p_memsz > p_filesz | 差值 = BSS，需要清零 |
| p_paddr | 段的目标物理地址 |
| Higher-half 转换 | 虚拟地址减去 0xFFFFFFFF80000000 得到物理地址 |
| Staging buffer | 0x1000000（16MB），存放从磁盘读取的原始 ELF 数据 |
| BIG_KERNEL_LBA | 848（磁盘上的大内核起始扇区） |
| BIG_KERNEL_MAX_SECTORS | 512（最多读取 256KB） |
| Bounds checking | `p_offset + p_filesz <= staging_size` |
