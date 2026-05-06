# 踩坑实录：三个让 Ring 3 起不来的致命 Bug

> 标签：debug、Triple Fault、页表权限、identity mapping、MSR、wrmsr
> 前置：[022-1 从 Ring 0 到 Ring 3](022-ring3-usermode-1.md)

## 前言

如果你跟着我们一路从 tag 000 写到了 tag 021，大概已经习惯了"写代码、编译、运行、看到预期输出"的节奏。但 tag 022 不是这样的——这个 tag 的开发过程可以用四个字来形容：血泪交加。我从写下 `jump_to_usermode()` 的第一行代码到最终看到 `CS=0x33, #GP at RIP=0x400000 from user mode (Ring 3)` 这行输出，中间经历了整整三天、三个独立 bug、无数次的 Triple Fault 和串口乱码。

这三个 bug 各自独立，每一个都足以让 Ring 3 完全起不来，而且它们之间会互相掩盖——修了 Bug 1 才能看到 Bug 2 的真实表现，修了 Bug 2 才能暴露 Bug 3。所以这篇文章不光是"踩坑记录"，更是一份 Ring 3 调试方法论。如果你也在写自己的 OS 内核、也在尝试进入 Ring 3，我希望这篇文章能帮你省下三天时间。

## Bug 排查路线图

在进入具体分析之前，先给一张"Ring 3 起不来"的排查路线图：

| 现象 | 第一个检查 | 第二个检查 | 可能的 bug |
|------|-----------|-----------|-----------|
| 串口输出乱码（`[`） | PDPT identity mapping | demand-page 重入 | Bug 1: FB mapping 丢失 |
| CS 异常（非 0x33） | STAR MSR EDX 值 | shlq 移位量 | Bug 2: wrmsr 只写 EDX |
| #PF error_code=0x05 | 每级页表 bit 2 | walk_level user_flag | Bug 3: 缺 FLAG_USER |
| SYSRET 后 #UD | EFER.SCE 位 | rdmsr 验证 | SCE 未启用 |
| Triple Fault | TSS.RSP0 | ltr 是否执行 | RSP0 未设或无效 |

注意：这张表假设你已经正确设置了 GDT 用户段描述符（tag 010）和 AddressSpace（tag 018）。

## 环境说明

调试环境：QEMU 8.x，KVM 加速（宿主机 Linux 6.x，x86-64），串口输出到 stdio。调试手段主要是串口 kprintf（因为 GDB 在特权级切换时的行为不太可靠），以及偶尔通过 QEMU Monitor 检查寄存器状态。

如果你正在调试自己的内核，建议优先使用 `-accel tcg -cpu max` 模式排除 KVM 干扰，再回到 KVM 模式验证性能。

## Bug 1：Framebuffer Identity Mapping 丢失

### 现象

`launch_first_user()` 执行后，串口输出变成这样：

```
[USER] Setting up first user-mode program...
[[[[[[[[[[[[[[[[VMM] Demand-paged 0x00000000FD08F000 -> phys 0x0000000001089000
V[[[[[[[[[[[[[
```

满屏的 `[` 字符，夹杂着 VMM 的 demand-page 日志。这显然不是我们想看到的东西。

### 根因

我们的 framebuffer 使用 identity mapping——物理地址直接当虚拟地址用，通过 PDPT[3] 的 1GB 大页映射在内核页表中。但 `AddressSpace` 构造时只复制 PML4 高半区条目（256-511），低半区全部清零。虽然 PML4[0] 本身会被复制（因为内核 PML4[0] 有指向 identity mapping PDPT 的条目），但新的 PDPT 是空的——1GB 大页映射丢失了。

当 SYSRET 进入 Ring 3 后，CR3 指向用户地址空间。此时 kprintf 写 framebuffer，MMU 查找 0xFD000000（framebuffer 的 identity mapping 地址），发现 PDPT[3] 是空的，触发 #PF。Demand-page handler 以为这是一个普通的不存在页，分配了一个新的物理页映射到这个地址。但这个物理页是普通 RAM，不是 MMIO——写入无效地址。更严重的是，page fault handler 内部又调用了 kprintf（打印 demand-page 日志），形成重入——kprintf 再次写 framebuffer，再次 #PF，再次 demand-page... 串口输出就被这些重入调用搞成了满屏方括号。

### 修复

在 `launch_first_user()` 中，activate() 之前把内核 PDPT 中已有的 identity mapping 条目复制到用户 PDPT：

```cpp
auto* kern_pdpt = reinterpret_cast<const uint64_t*>(
    (kern_pml4[0] & ADDR_MASK) + KERNEL_VMA);
auto* user_pdpt = reinterpret_cast<uint64_t*>(
    (user_pml4[0] & ADDR_MASK) + KERNEL_VMA);
for (uint32_t i = 0; i < PT_ENTRIES; i++) {
    if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT)) {
        user_pdpt[i] = kern_pdpt[i];
    }
}
```

只复制"内核有但用户没有"的条目，避免覆盖已有的用户映射。这段代码解决了 identity mapping 丢失的问题——切换 CR3 后 framebuffer 仍然可用，kprintf 不再触发 #PF 重入。

### 教训

这个 bug 的根本原因是 identity mapping 和地址空间隔离之间的矛盾。Identity mapping 把物理地址直接当虚拟地址用（在低半区），而地址空间隔离要求用户空间有自己的低半区页表。在"切换 CR3"这个操作中，所有低半区的映射都会丢失，除非显式复制。更好的方案是把 framebuffer 映射到高半区（像内核代码一样），这样 AddressSpace 构造时自动复制高半区条目就不需要额外处理了。但这是一个更大的重构，我们先用"复制 PDPT 条目"这个权宜之计。

## Bug 2：STAR MSR shlq 位数错误

### 现象

修了 Bug 1 后，串口不再乱码了，但 SYSRET 后 CS 寄存器变成了 0x10（内核代码段）而不是 0x33。CPU 没有切换到 Ring 3——它还在内核代码段上跑，特权级没变，接下来的行为取决于内存中 0x400000 地址处的内核映射内容。

### 根因

这完全是 `wrmsr` 指令语义的锅。MSR 寄存器宽 64 位，但 `wrmsr` 指令只写 EDX:EAX——EDX 写入 MSR 的高 32 位，EAX 写入低 32 位。注意是 EDX 和 EAX（32 位寄存器），不是 RDX 和 RAX（64 位寄存器）。

我们的原始代码是这样写的：

```asm
movq $0x23, %rdx
shlq $32, %rdx           # 期望把 0x23 移到 bit 63-32
orq  $0x10, %rdx
```

`shlq $32` 确实把 0x23 移到了 RDX 的 bit 39-32，所以 RDX = 0x0000002300000010。但 `wrmsr` 只读 EDX（bit 31-0），所以写入 STAR 高 32 位的值是 EDX = 0x00000010。STAR[63:48] = 0 而不是 0x23，SYSRET 计算出 CS = 0+16 = 0x10（内核代码段），CPU 不会切换到 Ring 3。

### 修复

把移位量从 $32 改为 $16：

```asm
movq $0x23, %rdx
shlq $16, %rdx           # EDX[31:16] = 0x23 -> STAR[63:48]
orq  $0x10, %rdx         # EDX[15:0]  = 0x10 -> STAR[47:32]
```

`shlq $16` 后 EDX = 0x00230010，`wrmsr` 把这个值写入 STAR 高 32 位，STAR[63:48] = 0x23，SYSRET 计算出 CS = 0x23+16 = 0x33（User64 代码段），SS = 0x23+8 = 0x2B（用户数据段）。正确了。

### 教训

x86 的 `wrmsr`/`rdmsr` 指令只操作 32 位寄存器（EDX:EAX），即使在 64 位模式下也是如此。如果你需要对 RDX 做移位操作来构造 MSR 值，一定要检查目标寄存器是 32 位的 EDX 还是 64 位的 RDX。这个 bug 的隐蔽之处在于它不会触发任何异常——只是静默地选择了错误的段选择子。

**如何避免这类 bug**：写 MSR 后立即用 rdmsr 读回并打印验证。如果你在 usermode_init_asm 末尾加一段 rdmsr 读 STAR 并通过串口打印 EDX 的值，就能在开发阶段立即发现 EDX 是 0x00000008 而不是预期的 0x00080008。这种"写后验证"的习惯在 MSR 编程中非常重要。

## Bug 3：walk_level 缺 FLAG_USER

### 现象

修了 Bug 1 和 Bug 2 后，SYSRET 正确进入了 Ring 3（CS=0x33），但立刻触发 #PF：

```
==== EXCEPTION: #PF (vector 14) ====
  ERROR CODE = 0x0000000000000005
```

error_code=0x05 意味着 P=1（页存在）、W/R=0（读操作）、U/S=1（Ring 3 用户态）。页存在但 Ring 3 没权限访问——典型的页表权限不足。

### 根因

x86-64 的四级页表权限检查机制非常严格：从 PML4 到 PT，每一级的页表项都必须设置 U/S 位（bit 2，即 FLAG_USER）。如果 PML4 条目没有 FLAG_USER，CPU 在 Ring 3 访问该虚拟地址时直接在第一级就被拒绝，即使后续的 PDPT/PD/PT 条目都有 FLAG_USER 也无济于事。

我们的 VMM 中，`walk_level` 函数在分配新的中间页表时只设了 `FLAG_PRESENT | FLAG_WRITABLE`，没有 `FLAG_USER`：

```cpp
// 原始代码——两处都是这样
entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;
```

所以即使用户态映射请求带了 FLAG_USER（最终 PT 条目有 FLAG_USER），中间的 PML4/PDPT/PD 条目都没有 FLAG_USER。CPU 在第一级检查就拒绝了 Ring 3 的访问。

### 修复

给 walk_level 增加 `user_flag` 参数，从 VMM::map() 中提取 `flags & FLAG_USER` 并传递给每一级：

```cpp
uint64_t user_flag = flags & FLAG_USER;
auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
auto* pd   = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
auto* pt   = walk_level(pd, PD_INDEX(virt), true, user_flag);
```

walk_level 内部分配新页表时加上 user_flag：

```cpp
entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
```

这样，如果 map() 的 flags 包含 FLAG_USER，每一级页表项都会有 user 位。内核态映射不受影响——不传 FLAG_USER，user_flag = 0。

### 教训

x86-64 的页表权限是逐级检查的，任何一级缺权限都会拒绝访问。这不是什么冷门知识点——Intel SDM Vol.3A Section 4.6 写得很清楚——但很容易被忽略，因为大多数简单的页表教程只关注最终 PT 条目的权限位，很少强调中间级别的权限传播。

## 设计对比：页表权限传播的不同实现

Linux 的页表映射路径（`__apply_to_page_range` 及相关函数）在分配中间页表时使用了 `pgtable_ops` 回调，其中包含权限传播逻辑。Linux 的 pte allocator 会根据最终的权限位设置中间页表的权限——本质上和 Cinux 的 `user_flag` 参数做的是同一件事，但 Linux 的实现更通用（通过函数指针而不是直接参数）。

xv6 是 32 位，只有两级页表（PD + PT），而且 xv6 的用户页直接使用 `mappages()` 函数，该函数在分配 PD 条目时没有显式传播 user 位——但 32 位 x86 的权限检查比 64 位宽松，而且 xv6 的 setupkvm() 在创建用户页表时直接从模板复制，不走 walk_level 这种通用路径。所以 xv6 没有遇到这个问题。

SerenityOS 使用了自己的 PageDirectory/PageTable 类层次结构，每个级别有自己的 allocate/reallocate 方法，权限传播是在类方法中处理的——比 Cinux 的过程式 walk_level 更面向对象，但核心机制相同。

## 三个 Bug 的共性

虽然这三个 Bug 涉及不同的子系统（页表、MSR、VMM），但它们有一个共同的主题：**x86-64 的权限检查是多层的，每一层都必须正确配置**。

- Bug 1（identity mapping 丢失）：地址空间切换时没有正确继承低半区映射——页表层级问题
- Bug 2（STAR MSR 移位错误）：MSR 写入只看 32 位 EDX 而非 64 位 RDX——寄存器宽度问题
- Bug 3（缺 FLAG_USER）：页表权限必须逐级传播——权限层级问题

这三者都是"x86-64 的某个检查是逐级/逐位进行的，而非只看最终结果"。这应该是你在写 OS 内核时需要牢记的核心原则：不要假设"最终结果对了就行"——x86-64 会检查路径上的每一个中间步骤。

## 收尾

三个 bug 全部修复后，Ring 3 终于能正确启动了。串口输出：

```
[USER] User address space activated (PML4 at phys 0x000000000106D000).
[USER] Jumping to Ring 3: entry=0x400000 stack=0x7FFFFF000

==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0x0000000000400000   CS  = 0x001b
[EXCEPTION] #GP at RIP=0x400000 from user mode (Ring 3)
[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!
```

CS=0x33（User64 代码段 Ring 3），RIP=0x400000（用户程序入口），用户程序执行 `cli` 触发 #GP。特权隔离验证成功。虽然过程曲折，但最终结果是对的——这三个 bug 让我们对 x86-64 特权级切换的每一个细节都有了更深的理解。

## 参考资料

- Intel SDM: Vol.3A Section 4.6 — Access Rights, U/S bit in paging structures
- Intel SDM: Vol.2A WRMSR/RDMSR — MSR 读写指令只操作 EDX:EAX
- OSDev Wiki: [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- Cinux notes: document/notes/022/001_usermode_three_bugs.md
- Cinux notes: document/notes/022/002_sfmask_qemu_msr.md
