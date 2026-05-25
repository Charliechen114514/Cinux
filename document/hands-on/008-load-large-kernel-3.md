---
title: 008-load-large-kernel-3 · 大内核加载
---

# 008-3 Hands-on: ELF64 解析器与大内核加载器

## 导语

前两节我们把 ATA PIO 磁盘驱动完全搞定了——从常量定义、辅助函数、初始化到扇区读取。mini kernel 现在能从磁盘读取任意扇区的数据。但光能读数据还不够——我们需要知道读出来的数据是什么格式、该怎么处理、最终要放到内存的什么位置。这就是 ELF 解析器要干的事情。

ELF（Executable and Linkable Format）是 Linux/Unix 系统的标准可执行文件格式，也是我们"大内核"在磁盘上的存储格式。大内核以 ELF64 形式编译，mini kernel 需要解析它的头部信息、找到所有需要加载的段（PT_LOAD）、把每个段搬到正确的物理地址、清零 BSS 区域，最后跳转到入口点执行。整个流程由大内核加载器编排——它先通过 ATA 驱动把整个 ELF 文件读到一个 staging buffer，然后调用 ELF 解析器完成加载。

完成本章后，我们会看到 mini kernel 成功从磁盘读取 mini kernel 自身的头部并验证 ELF magic，证明整个读取+解析管线已经就位。虽然真正的大内核还不存在（那是后续 milestone 的事），但加载器代码已经完全写好了。

---

## 概念精讲

### ELF 格式为什么是加载大内核的关键？

我们在之前的章节里，内核是被 Bootloader 以"裸二进制"（flat binary）的方式加载的——`objcopy -O binary` 把 ELF 文件里的所有内容按地址平铺成一个连续的二进制块，没有段信息，没有入口点信息。Bootloader 只能盲目地把它放到固定地址然后跳转。这种方式简单粗暴，但缺点也很明显：内核必须链接到一个固定的物理地址，而且所有段（代码、数据、BSS）必须连续排列。

ELF 格式就灵活得多。ELF 文件里有程序头表（Program Header Table），每个条目描述一个"段"（segment）的加载信息——它在文件中的偏移（p_offset）、要加载到的虚拟地址（p_vaddr）和物理地址（p_paddr）、在文件中占多少字节（p_filesz）、在内存中占多少字节（p_memsz）。特别是 p_memsz 可以大于 p_filesz，多出来的部分就是 BSS——需要清零但不需要从文件读取。

### ELF64 的关键结构

ELF64 文件的开头是 ELF header（64 字节），其中最重要的字段包括：e_ident（前 4 字节是 magic number `\x7FELF`）、e_type（文件类型，2 表示可执行文件）、e_machine（目标架构，0x3E 即 62 表示 x86-64）、e_entry（入口点虚拟地址）、e_phoff（程序头表在文件中的偏移）、e_phnum（程序头的数量）。

程序头（Program Header，56 字节）描述一个段的信息。我们只关心 p_type 为 1（PT_LOAD）的段。PT_LOAD 段有 p_offset（段数据在文件中的起始偏移）、p_vaddr（目标虚拟地址）、p_paddr（目标物理地址）、p_filesz（文件中的大小）、p_memsz（内存中的大小，大于 p_filesz 的部分是 BSS）。

### Higher-Half 内核的地址转换

Cinux 的大内核采用 higher-half 设计——内核代码链接在 0xFFFFFFFF80000000 以上的虚拟地址空间，但 mini kernel 运行时只有物理地址的身份映射。所以 ELF header 里的 e_entry 是虚拟地址，加载器需要减去 0xFFFFFFFF80000000 得到物理入口。

### Staging Buffer 与 Bounds Checking

大内核加载器先通过 ATA 驱动把整个 ELF 文件读到一个 staging buffer（物理地址 0x1000000，即 16MB），然后再由 ELF 解析器解析。我们事先不知道 ELF 文件有多大，所以读取一个"保守的上限"——512 个扇区（256KB）。ELF 加载器收到 staging buffer 的大小信息后，会检查每个段的 `p_offset + p_filesz` 是否超出 staging buffer 的范围，防止读到截断的垃圾数据。

---

## 动手实现

### Step 1: 实现 ELF64 解析器——头文件与结构体定义

**目标**：定义 ELF64 规范中的关键常量和数据结构。

**设计思路**：直接按照 ELF64 规范定义 Elf64_Ehdr（64 字节）和 Elf64_Phdr（56 字节），使用 `__attribute__((packed))` 防止编译器插入填充字节。

**踩坑预警**：ELF64 的程序头中 p_flags 在第二个字段（紧跟 p_type），而 ELF32 的 p_flags 在最后一个字段。搞混了会导致所有解析值全是垃圾。

**验证**：编译通过，`sizeof(Elf64_Ehdr)` 应该是 64，`sizeof(Elf64_Phdr)` 应该是 56。

---

### Step 2: 实现 ELF64 解析器——parse_elf_header()

**目标**：验证一个内存缓冲区包含合法的 ELF64 x86-64 可执行文件的头部。

**设计思路**：逐步检查 ELF header 的各个字段：magic number、64 位 class、little-endian、x86-64 架构、可执行类型、程序头表存在性。每步失败打印详细错误信息。函数不打印成功信息，因为调用者可能在探测。

**验证**：在 main.cpp 中，读取 LBA 16 调用 parse_elf_header 验证。函数不应崩溃。

---

### Step 3: 实现 ELF64 解析器——load_elf()

**目标**：解析 ELF 文件并将所有 PT_LOAD 段加载到正确的物理地址，返回入口点。

**设计思路**：先验证 ELF 头，然后遍历所有程序头。对每个 PT_LOAD 段做 bounds checking、memcpy 拷贝文件数据、memset 清零 BSS。最后做 higher-half 地址转换返回物理入口。

**踩坑预警**：bounds checking 非常重要——段数据可能超出实际读取的字节数，导致读到垃圾数据。higher-half 转换方向是虚拟→物理（减去基址）。

---

### Step 4: 实现大内核加载器——load_big_kernel()

**目标**：编排整个大内核加载流程——磁盘读取 + ELF 验证 + 段加载。

**实现约束**：常量 MINI_KERNEL_LOAD_ADDR = 0x20000, BIG_KERNEL_LOAD_ADDR = 0x1000000, BIG_KERNEL_LBA = 848, BIG_KERNEL_MAX_SECTORS = 512。`load_big_kernel(uint64_t disk_lba)` 返回入口点物理地址。

**踩坑预警**：BIG_KERNEL_LBA 必须和构建脚本中磁盘镜像的布局一致。ELF magic 的快速检查应该用逐字节比较避免对齐问题。

---

### Step 5: 更新 main.cpp 集成所有模块

**目标**：在 mini kernel 的主函数中初始化 ATA 驱动并运行 demo 测试。

**验证**：编译运行，完整串口输出应类似：

```
Cinux Mini Kernel v0.1.0
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[MINI] Milestone 008 complete. Waiting for big kernel (009+)...
```

---

## 调试技巧

### 常见 Bug 1: "No ELF magic at staging buffer"

staging buffer 的开头不是 ELF 文件。可能原因：BIG_KERNEL_LBA 值不对，或者磁盘镜像构建时大内核没被写入。用 `xxd` 检查磁盘镜像对应 LBA 位置的内容。

### 常见 Bug 2: 段加载后数据不对——triple fault

p_paddr 不在可访问的物理内存范围内，或者 memcpy/memset 有 bug。在 load_elf 中逐段打印目标地址和拷贝大小，用 QEMU monitor 的 `xp` 命令检查。

### 常见 Bug 3: BSS 没有被正确清零

memset 的参数传错了（count 用了 filesz 而不是 bss_size）。在 BSS 清零后立即读取目标地址验证是否为 0。

QEMU monitor 中检查内存：
```
xp /64bx 0x100000     # 查看物理地址 0x100000 处 64 字节
xp /16bx 0x1000000    # 查看 staging buffer 开头
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
| Higher-half 转换 | 虚拟地址减去 0xFFFFFFFF80000000 |
| Staging buffer | 0x1000000（16MB），原始 ELF 临时存放处 |
| BIG_KERNEL_LBA | 848（大内核起始扇区） |
| Bounds checking | `p_offset + p_filesz <= staging_size` |
