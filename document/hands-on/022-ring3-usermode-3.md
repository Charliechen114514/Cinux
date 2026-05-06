# 022-3 · 用户地址空间构建与特权隔离验证

## 导语

前两章我们搭好了 TSS 安全网和 SYSRET 跳板，现在要做的是构建用户态运行所需的完整环境，然后按下那个跳板。具体来说，我们需要创建一个独立的用户地址空间（有自己的 PML4），在里面映射用户代码页和用户栈页，把内核的 identity mapping 继承过来（不然进入 Ring 3 后连 framebuffer 都写不了），最后调用 jump_to_usermode 进入 Ring 3。如果一切正确，用户程序执行 `cli` 时会触发 #GP，串口输出特权隔离验证成功的消息。

这一章还涉及一个关键的 VMM 改造：`walk_level` 函数必须传播 FLAG_USER 到每一级页表项。x86-64 的权限检查遍历全部四级页表，任何一级缺 user 位都会拒绝 Ring 3 访问——这个坑非常经典。

**知识前置**：需要理解 AddressSpace 类（018_mm_address_space tag）的创建和使用方式，以及四级页表的基本结构（PML4 → PDPT → PD → PT → Page）。

**本章涉及的关键文件**：`kernel/mm/vmm.cpp`（walk_level FLAG_USER 传播）、`kernel/arch/x86_64/usermode.cpp`（usermode_init）、`kernel/proc/init.cpp`（kernel_init_thread 完整流程）、`kernel/main.cpp`（集成到 kernel_main）。

## 概念精讲

### 用户地址空间隔离

每个用户进程需要自己的页表。AddressSpace 构造时分配新的 PML4，高半区条目（256-511）从内核 PML4 复制——这样用户进程可以看到内核的代码、数据、堆、MMIO 映射。低半区条目（0-255）初始化为空，由用户进程自己映射用户代码和栈。

这种设计在现在的 Cinux 里有一个微妙的问题：我们的 framebuffer 使用 identity mapping——物理地址直接当虚拟地址用，通过 PDPT[3] 的 1GB 大页映射。但 AddressSpace 构造时只复制 PML4 高半区条目，PML4[0] 对应的低半区虽然被复制了（因为 kernel PML4[0] 有 identity mapping 的 PDPT），但新 PDPT 里的条目是空的——大页映射丢失了。进入 Ring 3 后，kprintf 写 framebuffer 时触发 #PF，demand-page 分配了一个普通 RAM 页覆盖了 MMIO 地址，导致串口输出变成乱码。

**为什么不只是复制 PML4[0]**：实际上 PML4[0] 已经被复制了（因为内核 PML4[0] 有值）。问题出在下一级——PML4[0] 指向的 PDPT 是新分配的空页，不是内核的 PDPT。AddressSpace 的构造函数只做"PML4 高半区复制"，不会递归复制 PDPT/PD/PT。所以即使 PML4[0] 指向了一个 PDPT，这个 PDPT 的内容是空的。

### FLAG_USER 的逐级传播

x86-64 的页表权限检查非常严格：从 PML4 到 PT，每一级的页表项都必须设置 U/S 位（bit 2，即 FLAG_USER）。如果 PML4 条目没有 FLAG_USER，CPU 在 Ring 3 访问该虚拟地址时直接在第一级就被拒绝，根本不会继续查 PDPT/PD/PT。即使最终的 PT 条目有 FLAG_USER 也无济于事。

Cinux 的 VMM 中，`walk_level` 函数负责遍历和分配页表。当某个中间级（PDPT/PD/PT）不存在时，它会分配一个新的物理页作为页表，设置 FLAG_PRESENT | FLAG_WRITABLE——但没有 FLAG_USER。所以即使用户态映射请求带了 FLAG_USER，中间页表页也不会有这个位。

修复方法很直接：给 walk_level 增加一个 `user_flag` 参数，从调用者传入的 flags 中提取 FLAG_USER 并传递下去。每次分配新页表时，除了 FLAG_PRESENT | FLAG_WRITABLE，还要 OR 上这个 user_flag。

### 用户程序的机器码

我们不是从磁盘加载 ELF 可执行文件（那是以后的事），而是在内核中直接硬编码一小段机器码字节。这段代码非常简单：`cli`（0xFA）清中断——这是一个特权指令，Ring 3 执行必定触发 #GP；`hlt`（0xF4）停机——也是特权指令；`jmp -4`（0xEB 0xFC）无限循环回 cli。总共 4 个字节。这段代码的意义不在于"做有用的事"，而在于证明特权隔离有效——#GP 被触发就说明 Ring 3 的权限限制在工作。

**为什么用机器码而不是 C 函数**：因为 C 函数需要编译器、链接器、C 运行时支持——这些东西在内核态的当前阶段还不具备为用户态编译代码的能力。机器码是最原始但最可靠的"用户程序"——4 个字节，不依赖任何运行时，不依赖栈布局，直接放在一个物理页上就能执行。后续 tag 会实现从 ELF 文件加载用户程序。

## 动手实现

### Step 1: 修改 walk_level 传播 FLAG_USER

**目标**：给 `walk_level` 函数增加一个默认为 0 的 `user_flag` 参数，在分配新的中间页表时传播这个标志。

**设计思路**：从 `VMM::map()` 中提取 `flags & FLAG_USER`，作为 `user_flag` 传给每一级的 walk_level 调用。walk_level 内部有两个地方分配新页表：一个是大页拆分路径（当遇到 1GB/2MB 大页但需要 4KB 映射时），另一个是空白条目分配新页表的路径。两处都需要在 `new_page | FLAG_PRESENT | FLAG_WRITABLE` 后面加上 `| user_flag`。

**实现约束**：`user_flag` 参数默认值为 0，这样内核态映射不受影响（不传 FLAG_USER，中间页表不会有 user 位）。只有当 map() 的 flags 包含 FLAG_USER 时，中间页表才会被标记为用户可访问。unmap 和 translate 路径不需要改——它们调 walk_level 时传 false 不分配新页表。

**踩坑预警**：这个 bug 的现象是 Ring 3 代码触发 #PF（error_code=0x05，P=1/W=0/U=1），意思是"页存在但权限不够"。如果你看到这个错误码，检查每一级页表项的 bit 2 是否为 1。

**验证**：编译后运行 host 单元测试，确认 FLAG_USER 的位值正确（0x7 = PRESENT|WRITABLE|USER）。内核测试中，创建 AddressSpace 并映射用户页后，translate 应该返回非零的物理地址。

### Step 2: 用户地址空间构建 — 构建并跳转到 Ring 3

**目标**：创建一个 AddressSpace，映射用户代码页（0x400000）和用户栈页（0x7FFFFF000 附近），复制 identity mapping，激活地址空间，设置 TSS.RSP0，然后调用 jump_to_usermode。

**设计思路**：整个流程分九步：(1) 创建 AddressSpace；(2) 分配用户代码物理页；(3) 在 USER_ENTRY_BASE (0x400000) 映射代码页；(4) 通过高半区直接映射把机器码字节复制到代码页；(5) 分配 USER_STACK_PAGES (4) 个物理页，映射到栈区域；(6) 复制内核 PDPT 的 identity mapping 到用户 PDPT；(7) activate() 切换 CR3；(8) 设置 TSS.RSP0 为当前内核栈；(9) 调用 jump_to_usermode（传入 `USER_STACK_TOP - USER_ABI_RSP_OFFSET` 作为栈顶，满足 x86_64 SysV ABI 的 16 字节对齐要求）。

**实现约束**：用户代码只有 4 个字节但需要整页映射（4KB），页的剩余部分是零。栈从高地址向低地址增长，所以 USER_STACK_TOP (0x7FFFFF000) 是栈顶（不包含在这个地址里），实际映射范围是 0x7FFFFB000 到 0x7FFFFEFFF（4 页 = 16KB）。identity mapping 复制只复制用户 PDPT 中不存在但内核 PDPT 中存在的条目，避免覆盖已有的映射。

**踩坑预警**：Step 6（identity mapping 复制）千万不能跳过。如果跳过了，进入 Ring 3 后 kprintf 写 framebuffer 触发 #PF，demand-page handler 尝试映射新页但 handler 内部又调用 kprintf，形成重入死循环——串口输出会变成无限重复的方括号 `[`。

**验证**：运行后预期输出：
```
[USER] Setting up first user-mode program...
[USER] User address space activated (PML4 at phys 0x...).
[USER] Jumping to Ring 3: entry=0x400000 stack=0x7FFFFF000
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0x400000   CS  = 0x0033
[EXCEPTION] #GP at RIP=0x400000 from user mode (Ring 3)
[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!
```

### Step 3: 修改 kernel_main 和 init 集成

**目标**：在 kernel_main 中调用 usermode_init()，在 kernel_init_thread 中构建用户地址空间并跳转到 Ring 3。

**设计思路**：kernel_main 在中断使能之后调用 usermode_init()（配置 STAR/EFER MSR 和 per-CPU GS 数据页）。实际的 Ring 3 跳转在 kernel_init_thread 中完成——创建 AddressSpace、映射用户代码和栈页、激活地址空间、设置 TSS.RSP0、然后调用 jump_to_usermode。

**验证**：完整构建运行后，看到上述的 Ring 3 #GP 输出。CS=0x33 确认 CPU 在 Ring 3（User64 代码段 + RPL3），RIP=0x400000 确认用户代码正确执行到了 cli 指令。

## 构建与运行

```bash
cd build && cmake .. && make big_kernel -j$(nproc)
```

```bash
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux.img,format=raw,index=0,media=disk
```

运行内核测试套件：

```bash
cd build && cmake .. && make big_kernel_test -j$(nproc)
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel tcg -cpu max -vga std \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux_test.img,format=raw,index=0,media=disk
```

Host 单元测试：

```bash
cd build && ctest --test-dir test -R usermode --output-on-failure
```

## 调试技巧

1. **串口输出乱码（无限 `[`）**：这是 identity mapping 丢失的典型症状。kprintf 写 framebuffer 触发 #PF，demand-page handler 内部又 kprintf，形成重入。检查 launch_first_user 中是否复制了内核 PDPT 的 identity mapping。
2. **#PF error_code=0x05**：P=1/W=0/U=1——页存在但 Ring 3 没权限。检查 walk_level 的 user_flag 参数是否正确传播。需要检查 PML4/PDPT/PD/PT 每一级的 bit 2。
3. **CS=0x13 而不是 0x33**：说明 STAR MSR 写入有误，SYSRET 计算出了错误的 CS。回顾第一章的 shlq 位数讨论——检查 STAR[63:48] 是否为 0x23。
4. **QEMU 上 SFMASK 测试失败**：QEMU 不持久化 SFMASK 写入，这是已知行为。测试应该改为验证 wrmsr 不触发 #GP，而不是验证 rdmsr 读回值。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 用户地址空间 | 独立 PML4，高半区复制内核，低半区映射用户代码/栈 |
| FLAG_USER 传播 | walk_level 必须在每一级传播 user 位 |
| Identity mapping 继承 | 切换 CR3 前复制内核 PDPT 条目到用户 PDPT |
| TSS.RSP0 | Ring 3 异常时 CPU 加载的内核栈指针 |
| 用户代码 | 4 字节机器码：cli; hlt; jmp -4 |
| #GP 验证 | CS=0x33(Ring 3) + RIP=0x400000 = 特权隔离成功 |

## 用户地址空间布局图

```
用户虚拟地址空间 (切换 CR3 后)
  0x0000000000000000 ┌──────────────────┐
                    │  (unmapped)      │
  0x0000000000400000 │  用户代码页       │  ← cli; hlt; jmp -4
  0x0000000000401000 │  (unmapped)      │
                    │  ...              │
  0x0000007FFFFB000 │  用户栈底         │  ← stack_base
  0x0000007FFFFC000 │  用户栈页 1       │
  0x0000007FFFFD000 │  用户栈页 2       │
  0x0000007FFFFE000 │  用户栈页 3       │
  0x0000007FFFFF000 │  (guard page)     │  ← USER_STACK_TOP (RSP)
  0x0000008000000000 ┼──────────────────┼  ← canonical 边界
  0xFFFF800000000000 │  内核映射 (复制)  │  ← PML4[256-511]
                    │  ...              │
  0xFFFFFFFFFFFFFFFF └──────────────────┘
```

## 关键常量速查

| 常量 | 值 | 含义 |
|------|----|------|
| USER_ENTRY_BASE | 0x400000 | 用户代码入口地址 |
| USER_STACK_TOP | 0x7FFFFF000 | 用户栈顶（RSP 初始值） |
| USER_STACK_PAGES | 4 | 栈页数（16KB） |
| stack_base | 0x7FFFFB000 | 栈映射起始地址 |
| kUserPageFlags | PRESENT|WRITABLE|USER (0x7) | 用户页权限 |
| KERNEL_VMA | 0xFFFFFFFF80000000 | 高半区偏移，用于 phys-to-virt |

## 常见错误排查清单

| 症状 | 可能原因 | 排查方法 |
|------|----------|----------|
| 串口输出乱码 `[` | identity mapping 丢失 | 检查 PDPT 条目复制 |
| #PF error_code=0x05 | walk_level 缺 FLAG_USER | 检查每一级 bit 2 |
| CS=0x13 或 0x10 | STAR MSR 写入错误 | 回顾 022-2 的 shlq 修复 |
| SFMASK 测试失败 | QEMU 不持久化 | 改为验证不触发 #GP |
| Triple Fault | TSS.RSP0 未设置 | 检查 tss_set_rsp0 调用 |

## 验证命令

构建并运行内核：

```bash
cd build && cmake .. && make big_kernel -j$(nproc)
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux.img,format=raw,index=0,media=disk
```

预期输出：
```
[BIG] GDT loaded (TSS with IST1 Double Fault stack).
[BIG] IDT loaded (#DF uses IST1).
[USER] STAR/EFER MSRs configured for SYSRET.
[USER] Setting up first user-mode program...
[USER] User address space activated (PML4 at phys 0x...).
[USER] Jumping to Ring 3: entry=0x400000 stack=0x7FFFFF000
==== EXCEPTION: #GP (vector 13) ====
  RIP = 0x400000  CS = 0x0033
[EXCEPTION] #GP at RIP=0x400000 from user mode (Ring 3)
[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!
```

运行内核测试：

```bash
cd build && cmake .. && make big_kernel_test -j$(nproc)
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel tcg -cpu max -vga std \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux_test.img,format=raw,index=0,media=disk
```

Host 单元测试：

```bash
cd build && ctest --test-dir test -R usermode --output-on-failure
```

## 参考资料

- Intel SDM: Vol.3A Section 4.6 — Access Rights, U/S bit in paging structures
- Intel SDM: Vol.3A Section 4.7 — Page-Table Entries
- OSDev Wiki: [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- Cinux notes: document/notes/022/001_usermode_three_bugs.md
- Cinux notes: document/notes/022/002_sfmask_qemu_msr.md
