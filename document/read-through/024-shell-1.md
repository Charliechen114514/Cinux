# 024-shell-1 Read-through: GDT 重构与 SYSCALL 路径修复

## 概览

本文覆盖 tag 024 中 GDT 布局重构和 SYSCALL 路径修复的完整代码。在整个 shell 功能中，这部分是底层基础——不修好 GDT selector 和寄存器保存，shell 根本跑不起来。关键设计决策包括：采用 Linux 兼容的 9 项 GDT 布局、将 RPL=3 编码进 STAR 基值、用 gs:16 scratch slot 替代 RBX 暂存返回值。

## 架构图

```
             SYSCALL 入口 (Ring 3 → Ring 0)
             ================================
用户态:      syscall指令
             ↓ RCX=saved RIP, R11=saved RFLAGS
             CS=STAR[47:32]=0x10, SS=STAR[47:32]+8=0x18
             ↓ swapgs
内核态:      syscall_entry (syscall.S)
             ↓ push trap frame (96 bytes)
             ↓ switch to kernel stack (gs:0)
             ↓ call syscall_dispatch
             ↓ save return value to gs:16
             ↓ restore user RBX from trap frame[80]
             ↓ restore RCX/R11 from trap frame
             ↓ switch to user stack (gs:8)
             ↓ restore return value from gs:16 to RAX
             ↓ swapgs
             SYSRETQ → Ring 3
             CS=STAR[63:48]+16=0x33, SS=STAR[63:48]+8=0x2B

             GDT Layout (9 entries)
             ======================
             Idx 0 (0x00): NULL
             Idx 1 (0x08): NULL (TLS placeholder)
             Idx 2 (0x10): Kernel Code  ← SYSCALL CS
             Idx 3 (0x18): Kernel Data  ← SYSCALL SS
             Idx 4 (0x20): User32 Code  ← STAR base target
             Idx 5 (0x28): User Data    ← SYSRETQ SS = 0x2B
             Idx 6 (0x30): User64 Code  ← SYSRETQ CS = 0x33
             Idx 7 (0x38): TSS low
             Idx 8 (0x40): TSS high
```

## 代码精讲

### gdt.hpp — Selector 常量与布局

```cpp
/// Linux-compatible GDT selector layout:
///   Idx 0: NULL
///   Idx 1 (0x08): unused (TLS placeholder)
///   Idx 2 (0x10): Kernel Code   ← SYSCALL CS
///   Idx 3 (0x18): Kernel Data   ← SYSCALL SS
///   Idx 4 (0x20): User32 Code   ← STAR[63:48] base for SYSRETQ
///   Idx 5 (0x28): User Data     ← SYSRETQ SS = 0x28|3 = 0x2B
///   Idx 6 (0x30): User64 Code   ← SYSRETQ CS = 0x30|3 = 0x33
///   Idx 7-8 (0x38): TSS (16 bytes)
constexpr uint16_t GDT_KERNEL_CODE = 0x10;
constexpr uint16_t GDT_KERNEL_DATA = 0x18;
constexpr uint16_t GDT_USER_CODE   = 0x33;
constexpr uint16_t GDT_USER_DATA   = 0x2B;
constexpr uint16_t GDT_TSS         = 0x38;
/// STAR[63:48] for SYSRETQ: using 0x23 so that +8=0x2B (SS with RPL=3 baked in)
/// and +16=0x33 (CS with RPL=3 baked in), avoiding reliance on CPU RPL-setting behavior.
constexpr uint16_t GDT_SYSRET_BASE = 0x23;
```

这段注释完整描述了新 GDT 布局的设计意图。每个 selector 的计算方式都直接标注了——你不需要翻手册就能理解为什么 GDT_USER_DATA 是 0x2B 而不是 0x28。GDT_SYSRET_BASE = 0x23 是整个修复的核心——它把 RPL=3 编码进了 STAR 基值，使得 SYSRETQ 的加法运算结果天然带 RPL=3。

selector 常量全部用 constexpr 定义，编译器会在所有引用处内联替换。这意味着改一个常量就能自动更新所有使用者。kEntryCount 从 7 增加到 9，为新增的 NULL 占位和 User64 Code 段腾出了空间。

### gdt.cpp — GDT 初始化

```cpp
void GDT::init() {
    entries_[0] = null_entry();

    // Idx 1 (0x08): unused (TLS placeholder)
    entries_[1] = null_entry();

    // Idx 2 (0x10): Kernel Code — SYSCALL CS
    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // Idx 3 (0x18): Kernel Data — SYSCALL SS
    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 4 (0x20): User32 Code — STAR[63:48] base for SYSRETQ
    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 5 (0x28): User Data — SYSRETQ SS = 0x28|3 = 0x2B
    entries_[5] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 6 (0x30): User64 Code — SYSRETQ CS = 0x30|3 = 0x33
    entries_[6] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // TSS at Idx 7-8
    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[7] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[8] = tss_high_entry(tss_addr);

    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);
}
```

注意 Idx 4 和 Idx 6 的 flags 区别——Idx 4 用 Size32（32 位兼容模式代码段），Idx 6 用 LongMode（64 位代码段）。SYSRETQ 返回 64 位模式时选择 Idx 6，返回兼容模式时选择 Idx 4。虽然 Cinux 不用兼容模式，但这个描述符必须存在且合法，否则 SYSRETQ 的 selector 加载会触发 #GP。

TSS 从旧的 Idx 5-6 移到了新的 Idx 7-8，selector 从 0x28 变成了 0x38。所有引用 GDT_TSS 的地方（主要是 IDT 的 IST 设置和 task switch 代码）都通过常量自动更新了。

### syscall.S — RBX 恢复修复

```asm
    call syscall_dispatch
    addq $8, %rsp                      # remove 7th arg from stack

    # Step 5: Save return value in GS scratch slot 2 (gs:16)
    #         Cannot use RBX -- it must be restored from the trap frame.
    movq %rax, %gs:16                  # save return value in per-CPU scratch

    # Step 6: Restore user state from trap frame
    movq 0(%rsp), %rax                 # frame+0: user RSP
    movq %rax, %gs:8                   # store user RSP for later restore
    movq 8(%rsp), %rcx                 # frame+8: user RIP (SYSRETQ loads from RCX)
    movq 16(%rsp), %r11                # frame+16: user RFLAGS
    movq 80(%rsp), %rbx                # frame+80: RESTORE user RBX (callee-saved)

    # Deallocate trap frame (12 slots * 8 bytes = 96 bytes)
    addq $96, %rsp
    movq %gs:8, %rsp                   # switch back to user stack

    # Step 8: Restore return value to RAX
    movq %gs:16, %rax                  # scratch → RAX: restore syscall return value

    # Step 9: SWAPGS back to user GS base
    swapgs

    # Step 10: Return to Ring 3
    sysretq
```

这段代码的修改点只有三处，但每一处都至关重要。第一处是把 `movq %rax, %rbx` 改成了 `movq %rax, %gs:16`——返回值不再暂存在 RBX，而是存在 GS 段的 scratch slot 中。第二处是新增了 `movq 80(%rsp), %rbx`——从 trap frame 的 offset 80 恢复用户态的 RBX 值。这个 offset 对应的是入口时 push RBX 的位置（按照 syscall.S 的 push 顺序：RSP、RIP、RFLAGS、RDI、RSI、RDX、RCX、RBX——RBX 是第 11 个 push，offset = 10*8 = 80）。第三处是把 `movq %rbx, %rax` 改成了 `movq %gs:16, %rax`。

这三处修改保证了 RBX 在 SYSCALL 调用前后的一致性。从用户态代码的角度看，SYSCALL 就像一次普通函数调用——callee-saved 寄存器不变，RAX 存返回值，RCX 和 R11 被破坏（ caller-saved ）。

### syscall.cpp — syscall_init 重构

```cpp
void register_builtin_handlers() {
    using namespace cinux::syscall;
    syscall_register(SyscallNr::SYS_read,  sys_read);
    syscall_register(SyscallNr::SYS_write, sys_write);
    syscall_register(SyscallNr::SYS_exit,  sys_exit);
    syscall_register(SyscallNr::SYS_yield, sys_yield);
}

void syscall_init() {
    uint64_t kernel_rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp));
    g_syscall_kernel_rsp = kernel_rsp;

    for (uint64_t i = 0; i < cinux::syscall::SYSCALL_TABLE_SIZE; i++) {
        cinux::arch::syscall_table[i] = nullptr;
    }

    constexpr uint32_t MSR_STAR  = 0xC0000081;
    constexpr uint32_t MSR_LSTAR = 0xC0000082;
    constexpr uint32_t MSR_SFMASK = 0xC0000084;

    uint64_t star_val = (static_cast<uint64_t>(GDT_SYSRET_BASE) << 48)
                      | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32);
    write_msr(MSR_STAR, star_val);

    uint64_t entry_addr = reinterpret_cast<uint64_t>(syscall_entry);
    write_msr(MSR_LSTAR, entry_addr);

    uint64_t sfmask_val = 0x200;
    write_msr(MSR_SFMASK, sfmask_val);

    register_builtin_handlers();

    kprintf("[SYSCALL] LSTAR=%p STAR configured SFMASK=0x200 (clear IF)\n",
            reinterpret_cast<void*>(entry_addr));
}
```

syscall_init 的变化有两点：一是签名从 `syscall_init(uint64_t kernel_rsp)` 变成了 `syscall_init()`（无参数），内部自己获取 RSP；二是在末尾调用 register_builtin_handlers() 自动注册所有 syscall handler。STAR 的值从旧版的 `(GDT_KERNEL_CODE << 32) | (GDT_KERNEL_CODE << 48)` 变成了 `(GDT_SYSRET_BASE << 48) | (GDT_KERNEL_CODE << 32)`——高 16 位用 GDT_SYSRET_BASE (0x23)，低 16 位用 GDT_KERNEL_CODE (0x10)。

main.cpp 中原本手动注册 handler 的代码被简化为一行 `cinux::arch::syscall_init()` 调用。新增 sys_read handler 时只需要在 register_builtin_handlers 中加一行，不需要改 main.cpp。

## 设计决策

### 决策：RPL 编码进 STAR 基值
**问题**: SYSRETQ 的 SS.RPL 在不同硬件/QEMU 版本上行为不一致
**本项目的做法**: STAR[63:48] = 0x23，RPL=3 编码在加法结果中
**备选方案**: 在 SYSCALL 出口手动 OR 0x03 到 SS selector
**为什么不选备选方案**: SYSRETQ 是硬件指令，我们无法在它执行后修改 SS——只能通过 STAR 基值间接控制
**如果要扩展**: 可以在 TSS 的 RSP0 切换时也使用类似技巧（把 RPL 编码进 selector）

### 决策：gs:16 作为返回值暂存
**问题**: RBX 被 SYSCALL 出口用作返回值暂存，但它是 callee-saved 寄存器
**本项目的做法**: 用 GS 段的第三个 slot（gs:16）暂存返回值
**备选方案**: 恢复整个 trap frame（包括所有 callee-saved 寄存器）
**为什么不选备选方案**: 当前 trap frame 只 push 了部分寄存器，完整恢复需要修改入口路径
**如果要扩展**: 应该在入口保存所有 callee-saved 寄存器（R12-R15），为未来复杂 syscall handler 做准备

## 扩展方向

- 在 SYSCALL 入口保存 R12-R15，防止未来内核代码破坏这些寄存器
- 使用 per-CPU 变量替代 GS 段的硬编码偏移，支持多核
- 在 TSS 中设置多个 IST 栈，让 #DF 和 #NMI 使用独立栈
- 添加 STAR MSR 的运行时断言，确保值在启动后不被意外修改

## 参考资料

- Intel SDM: Vol.2A SYSRET Instruction — CS = STAR[63:48]+16, SS = STAR[63:48]+8
  - URL: https://www.felixcloutier.com/x86/sysret
- Wikipedia x86-64: Intel vs AMD SYSRET RPL behavior divergence
  - URL: https://en.wikipedia.org/wiki/X86-64
- Linux kernel entry_64.S: 使用相同的 RPL 编码技巧
  - URL: https://android.googlesource.com/kernel/common/+/7fe05eede1c8945dad20ebc63564a80d6927ef06/arch/x86/entry/entry_64.S
