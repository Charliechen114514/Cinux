---
title: Ring 3 转换排查
---

# 022 · User Mode (Ring 3) 转换失败排查

## 现象

`launch_first_user()` 执行后串口输出乱码：

```
[USER] Setting up first user-mode program...
[[[[[[[[[[[[[[[[[VMM] Demand-paged 0x00000000FD08F000 -> phys 0x0000000001089000
V[[[[[[[[[[[[[
```

预期行为：SYSRET 进入 Ring 3 → 用户代码执行 `cli` → 触发 #GP → 打印寄存器转储并 halt。

## 根因分析

排查发现三个独立 bug，必须全部修复才能正确进入 Ring 3。

### Bug 1：Framebuffer identity mapping 在用户地址空间中丢失

**位置**：`kernel/arch/x86_64/usermode.cpp` — `launch_first_user()`

**原因**：Framebuffer 使用 identity mapping（`addr_ = fb_phys`，物理地址直接当虚拟地址用）。`map_mmio()` 通过 1GB 大页映射在 PDPT[3]。但 `AddressSpace` 构造时只复制 PML4 高半区条目（256–511），低半区全部清零。激活用户 CR3 后，PDPT[3] 的 1GB 页消失。

**后果**：`kprintf` 写 console 时触发 #PF → demand-page handler 映射了一个**普通 RAM 页**到该地址 → console 写入无效地址。更严重的是，page fault handler 内部再次调用 `kprintf`，形成**重入**，导致串口输出也被破坏（重复打印 `[` 字符）。

**修复**：在 `activate()` 之前，把内核 PDPT 中已有的 identity mapping 条目复制到用户 PDPT（仅复制用户 PDPT 中不存在的条目）：

```cpp
auto* kern_pdpt = phys_to_virt(kern_pml4[0].phys_addr());
auto* user_pdpt = phys_to_virt(user_pml4[0].phys_addr());
for (uint32_t i = 0; i < 512; i++) {
    if (kern_pdpt[i].present && !user_pdpt[i].present)
        user_pdpt[i] = kern_pdpt[i];
}
```

### Bug 2：STAR MSR 写入值错误（shlq $32 → shlq $16）

**位置**：`kernel/arch/x86_64/usermode.S` — `usermode_init_asm`

**原因**：`wrmsr` 指令只写 EDX:EAX（低 32 位），不写 RDX 的完整 64 位。代码用 `shlq $32, %rdx` 把 0x08 移到高 32 位，但 `wrmsr` 只读 EDX（低 32 位），高 32 位被丢弃。

```
预期：EDX = 0x00080008  →  STAR[63:48] = 0x08  →  CS = 0x1B
实际：EDX = 0x00000008  →  STAR[63:48] = 0x00  →  CS = 0x13（内核数据段 + RPL3）
```

**后果**：SYSRET 后 CS 加载为 0x13（数据段选择子），CPU 尝试在数据段上取指。首次运行时因 Bug 3 的 #PF 掩盖了此问题；修复 Bug 1、3 后表现为 CS=0x13 的 #PF。

**修复**：`shlq $32` → `shlq $16`，把值放在 EDX[31:16]。

### Bug 3：中间页表项缺少 FLAG_USER

**位置**：`kernel/mm/vmm.cpp` — `walk_level()`

**原因**：x86-64 四级页表中，**每一级**的条目都必须设置 user 位（bit 2），Ring 3 才能访问该页。`walk_level()` 分配新的 PDPT/PD/PT 页时只设了 `FLAG_PRESENT | FLAG_WRITABLE`，没有 `FLAG_USER`。即使最终 PT 项带了 FLAG_USER，中间某级缺 user 位仍会导致 #PF（protection violation）。

**错误码**：`error_code = 0x05`（P=1, W/R=0 read, U/S=1 user）——页存在但权限不足。

**修复**：`walk_level` 增加 `user_flag` 参数（默认 0），从 `VMM::map()` 的 flags 中提取 `FLAG_USER` 并向下传递：

```cpp
uint64_t user_flag = flags & FLAG_USER;
auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
auto* pd   = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
auto* pt   = walk_level(pd, PD_INDEX(virt), true, user_flag);
```

## 修复后输出

```
[USER] User address space activated (PML4 at phys 0x000000000106D000).
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000

==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0x0000000000400000   CS  = 0x001b
  ...
[EXCEPTION] #GP at RIP=0x0000000000400000 from user mode (Ring 3)
[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!
```

CS=0x1B（用户代码段 Ring 3），`cli` 在 Ring 3 触发 #GP，特权隔离验证成功。

## 教训

1. **`wrmsr` 只写 32 位**：x86 MSR 宽 64 位，但 `wrmsr` 指令只写 EDX:EAX，对 RDX 做移位操作时要注意目标寄存器是 32 位的 EDX 而非 64 位的 RDX。
2. **页表 user 位必须逐级传递**：x86-64 权限检查遍历全部四级页表，任何一级缺 user 位都会拒绝 Ring 3 访问。分配中间页表页时必须传播 FLAG_USER。
3. **identity mapping 与地址空间切换**：使用物理地址直当虚拟地址的 identity mapping 方案在切换 CR3 时容易断裂。地址空间隔离时应确保所有必需的 MMIO 映射都被继承或改用高半区映射。
