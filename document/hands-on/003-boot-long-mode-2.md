# 003-2 页表构建：setup_page_tables() 设计与实现

> 标签：x86_64, 长模式, 四级页表, 2MB 大页, 恒等映射, 高半内核
> 前置：[003-1 Long Mode 概念与页表设计](003-boot-long-mode-1.md)

## 本篇目标

上一篇我们把 Long Mode 的概念和页表设计理清楚了，现在该动手了。

本篇的核心任务是实现 `setup_page_tables` 函数——

在固定物理地址 0x1000-0x3FFF 上搭建一套三级页表结构（PML4 → PDPT → PD），

用 2MB 大页恒等映射前 8MB 物理内存，

同时建立 Higher-Half Kernel 映射

（PML4[511]→PDPT[510]→PD[0]，将虚拟地址 0xFFFFFFFF80000000 映射到物理地址 0x00000000）。

这个函数封装在 `boot/common/long_mode.S` 中，是整个 Long Mode 切换的前半段。

## Step 1: 创建 Long Mode 模块文件

**目标**：创建 `boot/common/long_mode.S` 文件，作为 Long Mode 切换的独立模块。

**设计思路**：

把 Long Mode 相关的函数（页表初始化、模式切换）从 stage2.S 中独立出来，

放到 common 目录下作为单独的汇编文件。

这样做的好处是 stage2.S 不会继续膨胀，

而且 Long Mode 的初始化逻辑可以被其他启动路径复用。

这个文件需要在头部用 `.code32` 标记，因为它是在保护模式下被调用的。

**实现约束**：

文件需要定义两组常量。

第一组是页表的物理地址布局——

PML4 放在 0x1000、PDPT 放在 0x2000、PD 放在 0x3000，

这是固定地址，因为 Bootloader 阶段没有动态内存分配。

第二组是控制位掩码——

- PAGE_PRESENT（0x01）：页表条目的存在位
- PAGE_WRITABLE（0x02）：读写位
- PAGE_LARGE（0x80）：大页标志位（2MB 页）
- MSR_EFER（0xC0000080）：EFER 寄存器的 MSR 地址
- EFER_LME（0x100）：Long Mode Enable 位的值
- CR4_PAE（0x20）：PAE 位的掩码
- CR0_PG（0x80000000）：分页使能位
- CR0_PE（0x01）：保护模式使能位

另外还需要定义 DEBUGCON_PORT（0xE9）和 CHAR_LONG_MODE（0x4C，即字符 'L'）用于调试输出。

**踩坑预警**：

EFER_LME 的值是 0x100（bit 8），绝对不能写成 0x1000。

0x1000 对应的是 bit 12，即 AMD 的 SVME（Secure Virtual Machine Enable）位。

如果写错了，CPU 不会报错——`wrmsr` 会很乖地设置 SVME 位而不是 LME 位——

但当你随后开启分页时，CPU 发现 LME 没设却开了分页，直接三重故障。

这个坑是我们在开发时实打实踩过的，

GDB 里看到 EFER=0x1000 只有 SVME 没有 LME，定位了半天才发现是常量定义写错了一位。

调试这种问题时，在 GDB 里用 `info registers` 查看 CR0、CR4 和 EFER 的值是最直接的排查手段。

**验证**：此时只需要确认文件被创建、常量定义正确即可，构建验证在后续步骤完成。

## Step 2: 实现页表清零逻辑

**目标**：清零 PML4（0x1000）、PDPT（0x2000）、PD（0x3000）三张页表。

**设计思路**：

页表初始化的第一步是清零——

确保所有条目初始值为 0，这样未使用的条目的 Present 位就是 0，CPU 不会把残留的垃圾数据当成有效条目。

三张表各占 4KB（即 1024 个 32 位双字），

用 `rep stosl` 指令批量写入零值来清零。

`rep stosl` 的意思是：把 EAX 的值（这里是 0）重复写入 ES:EDI 指向的内存地址 ECX 次，

每次写入后 EDI 自动递增 4。

函数开头需要先用 `cld` 指令清除方向标志位，

确保 `stosl` 是递增写入而非递减——

虽然正常情况下 DF 应该是 0，但在函数入口再确认一次是防御性编程。

**实现约束**：

函数需要依次清零三个地址。

每次清零操作都是设置 EDI 为目标地址、EAX 为 0、ECX 为 1024，然后执行 `rep stosl`。

页表条目是 64 位的，但我们用 32 位的 `stosl` 写 0 来清零——

连续写两次 32 位的 0 等价于写一次 64 位的 0，所以没问题。

函数会修改 EAX、ECX、EDI 寄存器。

**验证**：

在 GDB 中可以在 `setup_page_tables` 的清零完成后设断点，

用 `x/8gx 0x1000`、`x/8gx 0x2000`、`x/8gx 0x3000` 查看三张表的内容，应该全部为 0。

## Step 3: 建立层级链接

**目标**：设置 PML4[0] → PDPT 和 PDPT[0] → PD 的链接关系。

**设计思路**：

清零完成后，需要建立三级页表之间的链接。

PML4 的第 0 个条目写入 PDPT 的物理地址（0x2000）加上 Present+Writable 标志（0x03），

PDPT 的第 0 个条目写入 PD 的物理地址（0x3000）加上同样的标志。

这样 CPU 在做地址翻译时，

从 CR3 找到 PML4，从 PML4[0] 找到 PDPT，从 PDPT[0] 找到 PD。

为什么只设置第 0 个条目？

因为 PML4[0] 覆盖的虚拟地址范围是 0x0000000000000000 到 0x0000007FFFFFFFFF

（即第一个 512GB 区域），PDPT[0] 覆盖的是这个 512GB 区域中的第一个 1GB，对 Bootloader 阶段完全足够。

**实现约束**：

用 `movl` 把下一级页表物理地址加上 0x03 标志写入当前级页表的第 0 个条目。

注意这里用 `movl` 做的是 32 位写入，只写了条目的低 32 位，

高 32 位保持为 0（之前清零了）——

对于 8MB 以下的物理地址这是正确的，

Intel SDM Vol.3A §4.5 要求页表条目中未实现的保留位必须为 0。

**验证**：

GDB 中检查 PML4[0] = 0x2003（PDPT 地址 + 标志），PDPT[0] = 0x3003（PD 地址 + 标志）。

## Step 4: 填充 PD 大页条目——恒等映射

**目标**：填充 PD 的前 4 个条目，每个条目映射一个 2MB 大页，实现前 8MB 物理内存的恒等映射。

**设计思路**：

PD 条目的 bit 7（PS，Page Size）设为 1 表示这是一个 2MB 大页，

bit 0（Present）和 bit 1（Writable）也设为 1。

第 i 个条目的物理基地址等于 i 左移 21 位（即 i × 2MB）。具体来说：

- PD[0] 映射 0x00000000-0x001FFFFF（0-2MB）
- PD[1] 映射 0x00200000-0x003FFFFF（2-4MB）
- PD[2] 映射 0x00400000-0x005FFFFF（4-6MB）
- PD[3] 映射 0x00600000-0x007FFFFF（6-8MB）

总共 8MB 的恒等映射，对 Bootloader 阶段绰绰有余。

**实现约束**：

用一个循环实现——

EDI 指向 PD 起始地址，ECX=4 控制循环次数，EAX 从 0 开始作为页索引。

每次迭代把 EAX 左移 21 位得到物理基地址，OR 上 0x83 标志，写入 EDI 指向的位置，

然后 EDI 前进 8 字节（条目大小），EAX 自增。

**验证**：

GDB 中用 `x/8gx 0x3000` 查看 PD，

PD[0..3] 分别是 0x83、0x200083、0x400083、0x600083。

## Step 5: 建立 Higher-Half Kernel 映射

**目标**：设置 PML4[511] → PDPT 和 PDPT[510] → PD 的链接，实现 Higher-Half Kernel 映射。

**设计思路**：

恒等映射只能让内核跑在低地址。

为了让内核运行在高半地址空间（0xFFFFFFFF80000000 以上），我们需要额外的映射。

虚拟地址 0xFFFFFFFF80000000 的拆解是：

- PML4 索引 = bit 47:39 = 0x1FF = 511
- PDPT 索引 = bit 38:30 = 0x1FE = 510（注意是 510 而不是 511，因为 bit 30 为 0）
- PD 索引 = bit 29:21 = 0x000 = 0

所以我们需要设置 PML4[511] 指向 PDPT，PDPT[510] 指向 PD。

PD[0] 就是那条映射 0-2MB 的大页条目，已经在 Step 4 中设好了——

所以 Higher-Half 映射只需要添加两个链接条目即可。

**实现约束**：

PML4[511] 写入 PDPT 物理地址 + 0x03 标志（和 PML4[0] 一样），

PDPT[510] 写入 PD 物理地址 + 0x03 标志（和 PDPT[0] 一样）。

但是！这里有一个关键区别：

因为 x86-64 页表条目是 64 位的，而且 PML4[511] 和 PDPT[510] 是"非常用"条目，

虽然物理地址在 4GB 以下高 32 位应该为 0，

但为了安全起见，必须显式地把高 32 位也写为 0。

具体来说，先写低 32 位（物理地址 + 标志），再写高 32 位为 0。

PML4[511] 的偏移是 PML4_PHYS_ADDR + (511 * 8)，

PDPT[510] 的偏移是 PDPT_PHYS_ADDR + (510 * 8)。

**踩坑预警**：

PDPT 索引是 510 不是 511。

如果你把 PDPT[511] 设成指向 PD，

那对应的虚拟地址是 0xFFFFFFFFC0000000 而不是 0xFFFFFFFF80000000——差了整整 1GB。

这个 off-by-one 错误在调试时非常难发现，

因为 CPU 不会报错，只是映射到了错误的地址。

我们是通过计算虚拟地址的二进制拆分才确认正确的索引是 510。

**验证**：

GDB 中检查 PML4[511] = 0x2003（PDPT 地址 + 标志），PDPT[510] = 0x3003（PD 地址 + 标志）。

## 构建验证

完成以上步骤后，`setup_page_tables` 函数在 `ret` 返回时，

0x1000-0x3FFF 处的页表结构应该完整建立。

在 GDB 中可以用以下命令验证完整的页表内容：

```
(gdb) x/8gx 0x1000    # PML4: [0]=0x2003, [511]=0x2003
(gdb) x/8gx 0x2000    # PDPT: [0]=0x3003, [510]=0x3003
(gdb) x/8gx 0x3000    # PD: [0]=0x83, [1]=0x200083, [2]=0x400083, [3]=0x600083
```

下一篇我们将实现 `enter_long_mode` 函数，利用这套页表把 CPU 真正带入 64 位 Long Mode。

## 参考资料

- Intel SDM Vol.3A §4.1 — Paging Overview：Long Mode 下分页的强制性
- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD/PT 层级结构
- Intel SDM Vol.3A §4.5.1 — Page Size Extensions：2MB 和 1GB 大页
- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：初始化步骤
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
