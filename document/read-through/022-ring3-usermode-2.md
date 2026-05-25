---
title: 022-ring3-usermode-2 · 用户态 (Ring 3)
---

# 022-2 Read-through · usermode.S + usermode.cpp 完整代码精讲

## 概览

本文精讲 tag 022 的核心文件——usermode.S（汇编）和 usermode.cpp（C++）。usermode.S 包含两个汇编函数：`usermode_init_asm` 配置 STAR/EFER/SFMASK 三个 MSR，`jump_to_usermode` 执行 SYSRET 从 Ring 0 切换到 Ring 3。usermode.cpp 提供 `usermode_init()` 函数，调用汇编初始化、打印日志并分配 per-CPU GS 数据页。用户地址空间的构建和 Ring 3 跳转在 `kernel_init_thread()`（kernel/proc/init.cpp）中完成。

关键设计决策：使用 SYSRET（而非 iret）进入 Ring 3；用户代码硬编码为 4 字节机器码（而非从 ELF 文件加载）；用户栈 16KB（4 页）位于 0x7FFFFF000 以下。

## 架构图

```
kernel_main()
  |
  +-> usermode_init()
  |     +-> usermode_init_asm()     [usermode.S]
  |           - wrmsr STAR          (0xC0000081)
  |           - wrmsr SFMASK        (0xC0000084)
  |           - rdmsr+or+wrmsr EFER (0xC0000080)
  |
  +-> kernel_init_thread()          [proc/init.cpp]
        |
        +-> AddressSpace user_space  (new PML4)
        +-> map code page @ 0x400000 (FLAG_USER)
        +-> copy machine code bytes
        +-> map stack pages @ 0x7FFFFB000-0x7FFFFEFFF
        +-> copy identity mapping (PDPT)
        +-> user_space.activate()    (switch CR3)
        +-> GDT::tss_set_rsp0(rsp)   (set kernel stack)
        +-> jump_to_usermode()       [usermode.S]
              - RCX = 0x400000
              - RSP = 0x7FFFFF000
              - R11 = 0x202 (RFLAGS)
              - sysretq --> Ring 3!
```

## 代码精讲

### usermode.hpp — 常量和接口声明

头文件定义了三个关键常量：

```cpp
constexpr uint64_t USER_ENTRY_BASE = 0x400000;      // 用户代码入口
constexpr uint64_t USER_STACK_TOP = 0x7FFFFF000;    // 用户栈顶
constexpr uint64_t USER_STACK_PAGES = 4;             // 4 页 = 16KB
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;          // x86_64 SysV ABI RSP 对齐偏移
```

`0x400000` 是 x86-64 ELF 的标准加载地址（4MB），Linux 用户程序的默认 text 段基址也是这个值。`0x7FFFFF000` 接近 2GB 边界但留了一页的 guard page。`USER_ABI_RSP_OFFSET = 8` 用于满足 x86_64 SysV ABI 的栈对齐要求——实际传给 SYSRET 的 RSP 是 `USER_STACK_TOP - 8`，使得 RSP 在入口点满足 16 字节对齐的 `RSP % 16 == 8` 约定（模拟 `call` 指令压入返回地址的效果）。这些地址都在用户空间（低于 0x800000000000 canonical 边界），Ring 3 可以访问。

头文件还声明了 public 函数和 extern "C" 的汇编入口：

```cpp
void usermode_init();
```

extern "C" 块中的 jump_to_usermode 使用 C 链接避免 name mangling，参数通过 RDI/RSI/RDX 传入（System V AMD64 ABI）。注意：jump_to_usermode 调用后不会返回——SYSRET 把控制权交给了 Ring 3 代码。

### usermode.S — usermode_init_asm 汇编

这是 MSR 配置函数，设置 SYSRET/SYSCALL 所需的三个 MSR。

STAR MSR 写入是整个 tag 中最微妙的操作：

```asm
movq $0x23, %rdx           # SYSRET 基选择子（含 RPL=3）
shlq $16, %rdx             # 移到 EDX[31:16] -> STAR[63:48]
orq  $0x10, %rdx           # EDX[15:0] -> STAR[47:32]（内核 CS 基）
xorq %rax, %rax            # EAX = 0 (低 32 位未使用)
movq $0xC0000081, %rcx     # MSR_STAR
wrmsr                       # EDX:EAX -> MSR
```

这里 `shlq $16` 把 0x23 移到 EDX 的 bit 31-16，所以 EDX = 0x00230010。wrmsr 把 EDX:EAX 写入 STAR MSR，结果 STAR = 0x00230010_00000000。STAR[63:48] = 0x23（SYSRET 基选择子，已含 RPL=3），STAR[47:32] = 0x10（SYSCALL 内核 CS 基）。SYSRET 由此计算 CS = 0x23+16 = 0x33（User64 代码段），SS = 0x23+8 = 0x2B（用户数据段）。

注意这里必须用 `shlq $16` 而不是 `shlq $32`。`wrmsr` 只读取 EDX（RDX 低 32 位）和 EAX（RAX 低 32 位），RDX 高 32 位完全被忽略。如果用 `shlq $32`，0x23 被移到了 RDX 的 bit 39-32，但 EDX（bit 31-0）仍然是 0，所以 STAR[63:48] = 0，SYSRET 计算出 CS = 0+16 = 0x10（内核代码段），CPU 不会切换到 Ring 3。

SFMASK 写入：

```asm
movq $0xC0000084, %rcx     # MSR_SFMASK
xorq %rdx, %rdx            # EDX = 0
movq $0x200, %rax           # EAX = 0x200 (IF bit mask)
wrmsr
```

SFMASK 值 0x200 表示 SYSCALL 时自动清除 RFLAGS 的 IF 位（关中断）。但这只在 SYSCALL 方向生效——SYSRET 从 R11 恢复 RFLAGS，不受 SFMASK 影响。注意 QEMU 不持久化 SFMASK 的写入——`wrmsr` 不报错，但 `rdmsr` 读回始终是 0。这在 KVM 和 TCG 两种后端上都能复现。

EFER.SCE 启用：

```asm
movq $0xC0000080, %rcx     # MSR_EFER
rdmsr                        # 读当前 EFER
orq  $1, %rax               # 设置 SCE (bit 0)
wrmsr                        # 写回
```

必须先读后写——EFER 包含 LMA（Long Mode Active）、NXE（No-Execute Enable）等重要位，直接覆盖会导致这些位丢失。`orq $1` 只设置 SCE 位，其余位保持不变。

### usermode.S — jump_to_usermode 汇编

这是 Ring 0 到 Ring 3 的跳转函数：

```asm
jump_to_usermode:
    movq %rdi, %rcx            # entry -> RCX (SYSRET loads into RIP)
    movq %rsi, %rsp            # user_stack -> RSP
    movq %rdx, %rdi            # arg -> RDI (用户程序第一个参数)
    pushq $0x202               # RFLAGS: IF(bit9) + reserved bit1
    popq  %r11                 # -> R11 (SYSRET loads into RFLAGS)
```

前三行设置 SYSRET 的三个关键寄存器：RCX 是目标 RIP（用户入口地址），RSP 是用户栈顶，RDI 是传给用户程序的参数。R11 通过 push/pop 设置为 0x202——bit 9 是 IF（中断使能），bit 1 是 RFLAGS 的保留位（必须为 1）。SYSRET 后 RFLAGS 从 R11 恢复，同时清除 RF、VM 等调试/虚拟化相关位，并将 bit 1 强制设为 1。我们设置的 IF (bit 9) 会被保留，所以用户态下中断是开启的。SYSRET 后 CS = STAR[63:48]+16 = 0x33（User64 代码段），SS = STAR[63:48]+8 = 0x2B（用户数据段）。

接下来清零所有通用寄存器：

```asm
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rbp, %rbp
    xorq %r8, %r8
    ... (R9-R15 全部清零)
    sysretq                    # 进入 Ring 3!
```

这一步看似多余，但实际上非常重要。进入 Ring 3 前如果不清零寄存器，用户代码可以读到内核的 RSP（栈指针）、物理页地址等敏感信息。虽然当前的"用户程序"只是 cli;hlt 死循环不会读寄存器，但这是一种安全卫生习惯。Linux 的 entry 代码也做同样的事情。

`sysretq` 是关键指令——`q` 后缀表示 64 位操作数大小（REX.W 前缀），确保返回 64 位模式而非 32 位兼容模式。执行后 CPU 的 CPL 变为 3，RIP = RCX = 0x400000，RFLAGS = R11 & mask | 2，CS = 0x33，SS = 0x2B。

### usermode.cpp — usermode_init() + GS 数据页分配

这是 C++ 层面的初始化函数。它调用汇编的 `usermode_init_asm()` 配置 STAR/EFER MSR，打印确认日志，然后从 PMM 分配一个物理页作为 per-CPU GS 数据页（通过 wrmsr 写入 KERNEL_GS_BASE MSR），用于后续 syscall 入口的内核栈访问。

```cpp
void usermode_init() {
    usermode_init_asm();
    kprintf("[USER] STAR/EFER MSRs configured for SYSRET.\n");

    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    uint64_t gs_phys = g_pmm.alloc_page();
    auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
    gs_virt[0] = 0;  // kernel stack — filled by scheduler
    gs_virt[1] = 0;
    write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);
    // ...
}
```

GS 数据页的布局：`gs:0` 保存内核栈指针（调度器在上下文切换时更新），`gs:8` 保存用户 RSP 暂存（syscall handler 使用），`gs:16` 保存返回值暂存。这个页在后续 SYSCALL 入口代码中通过 `swapgs; movq %gs:0, %rsp` 获取内核栈。

用户代码的机器码和地址空间构建逻辑不在 usermode.cpp 中，而是在 `proc/init.cpp` 的 `kernel_init_thread()` 里。那里通过 fork + execve 加载 shell 程序到 Ring 3 运行。用户地址空间构建的核心流程是：创建 AddressSpace，分配代码页并映射到 0x400000，通过高半区直接映射写入 ELF 代码段，分配并映射 4 页栈空间，复制 identity mapping，激活地址空间，设置 TSS.RSP0，调用 jump_to_usermode。

Identity mapping 复制是最容易遗漏的步骤：

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

只复制"内核有但用户没有"的 PDPT 条目。framebuffer 的 1GB 大页在 PDPT[3]（低半区），AddressSpace 构造时虽然复制了 PML4[0]（指向 PDPT），但新 PDPT 的条目是空的。这段代码把内核 PDPT 的大页映射补到用户 PDPT 中，确保切换 CR3 后 framebuffer 仍然可用。

## 设计决策

### 决策：SYSRET vs iret 进入 Ring 3

**问题**：用哪种方式从 Ring 0 切换到 Ring 3？

**本项目的做法**：SYSRET（通过 usermode.S 中的 sysretq）。

**备选方案**：iret（构造中断栈帧，push SS/RSP/RFLAGS/CS/RIP 后 iret）。

**为什么不选备选方案**：SYSRET 是 x86-64 长模式下专用的快速系统调用/返回指令，与后续的 SYSCALL 系统调用机制 (tag 023) 天然配对。iret 虽然更通用（可以返回任意特权级），但 SYSRET 更简洁——只需要设置 RCX 和 R11 两个寄存器。xv6 使用 iret（因为是 32 位），Linux 使用 SYSRET（64 位）。

**如果要扩展/改进**：实际的操作系统中，SYSRET 用于系统调用返回（快速路径），iret 用于异常/中断返回和信号投递（通用路径）。Cinux 在后续 tag 中会同时支持两种。

## 扩展方向

- 添加 SWAPGS 支持（SYSRET 前后交换 GS base）
- 支持从 ELF 文件加载用户程序（而非硬编码机器码）
- 实现用户程序的参数传递和退出码
- 添加用户地址空间的 guard page（栈下方的不可访问页）

## 参考资料

- Intel SDM: Vol.2A SYSRET — SYSRET 指令操作伪代码，STAR MSR 计算 CS/SS
- Intel SDM: Vol.4 — STAR (0xC0000081), EFER (0xC0000080), SFMASK (0xC0000084)
- OSDev Wiki: [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- felixcloutier.com: [SYSRET](https://www.felixcloutier.com/x86/sysret)
- Linux: arch/x86/entry/entry_64.S — SYSRET return path
- Cinux notes: document/notes/022/001_usermode_three_bugs.md
- Cinux notes: document/notes/022/002_sfmask_qemu_msr.md
