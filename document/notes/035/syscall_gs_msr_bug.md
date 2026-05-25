---
title: Syscall GS MSR 丢失
---

# 035 syscall GS MSR 状态丢失排查报告

## 现象

修复 fork 帧指针 bug 后，shell 子进程 execve 成功进入用户态，但执行第一条 syscall 时崩溃：

```
[EXECVE] loaded /bin/sh entry=0x0000000000400260 pid=1
[GUI] Shell child jumping to user mode: entry=0x0000000000400260
[VMM] Demand-paged 0x0000000000000000 -> phys 0x00000000014DA000

==== EXCEPTION: #DF (vector 8) ====
  RIP   = 0xFFFFFFFF8100E88B   CS  = 0x0010
  RSP   = 0x0000000000000000   SS  = 0x0018
  ERROR CODE = 0
[FATAL] Double Fault -- halting.
```

关键线索：
- **Demand-page at 0x0**：内核在虚拟地址 0 触发缺页
- **RSP = 0**：Double Fault 帧中 RSP 为零
- 崩溃前只有 1 次 demand-page（之前修复的版本是 5 次 + #GP）

## syscall_entry 的 GS 机制

syscall.S 的入口使用 `swapgs` + GS 段寻址访问 per-CPU 数据：

```asm
syscall_entry:
    swapgs                         # 交换 MSR_GS_BASE ↔ MSR_KERNEL_GS_BASE
    movq %rsp, %gs:8              # 保存用户 RSP 到 per-CPU 数据页
    movq %gs:0, %rsp              # 从 per-CPU 数据页加载内核栈
    ...
```

`swapgs` 的正确前置条件：
- **用户态**：`MSR_GS_BASE = 0`，`MSR_KERNEL_GS_BASE = per-CPU 数据页地址`
- `swapgs` 后：`MSR_GS_BASE = per-CPU 数据页地址` → `gs:0` 读到内核栈

## 根因

### 调度器不保存/恢复 GS MSR

MSR_GS_BASE 和 MSR_KERNEL_GS_BASE 是 CPU 全局寄存器，不随任务切换自动保存。调度器的 `context_switch` 只保存 callee-saved 寄存器（R15-RBX, RSP, RIP），**不保存 GS MSR**。

### 崩溃链条

```
1. 第一个 shell 调 sys_read(stdin) → 管道空 → 阻塞
   此时 syscall_entry 已执行 swapgs：
     MSR_GS_BASE = per-CPU 数据页    ← 已交换
     MSR_KERNEL_GS_BASE = 0          ← 已交换

2. 阻塞触发 schedule() → context_switch 切换到 gui_worker
   调度器不保存/恢复 GS MSR → gui_worker 继承了交换后的 GS 状态

3. gui_worker 调 fork() → 子进程被加入调度队列
   子进程 CpuContext 由 memcpy(parent) 初始化
   → ctx.gs_base = per-CPU 数据页（父进程当时的值）
   → ctx.kgs_base = 0

4. 调度器切换到子进程 → context_switch 恢复子进程的 GS MSR：
     MSR_GS_BASE = per-CPU 数据页    ← 错误！应该是 0
     MSR_KERNEL_GS_BASE = 0          ← 错误！应该是 per-CPU 数据页

5. 子进程执行 child path → jump_to_usermode → SYSRETQ
   SYSRETQ 不改变 GS → 用户态 MSR_GS_BASE 仍为 per-CPU 数据页

6. Shell 子进程执行 sys_write → SYSCALL → syscall_entry：
   swapgs → MSR_GS_BASE = 0, MSR_KERNEL_GS_BASE = 0
   movq %gs:0, %rsp → 读虚拟地址 0 → 缺页！

7. demand-page 在地址 0 映射了一页，gs:0 读到垃圾值（可能是 0）
   → RSP = 0 → 下一次 push → 非规范地址 → #GP
   → #GP 处理时 RSP 仍为 0 → 无法 push → Double Fault
```

### 为什么 launch_first_user 不受影响

`launch_first_user()` 是系统启动时唯一在内核态初始化 GS 的路径：

```cpp
write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);
// 此时 MSR_GS_BASE = 0（从未显式设置）
```

启动后 GS 状态正确，且没有其他任务与之竞争，所以第一个 shell 运行正常。问题只在多任务调度 + fork 场景下才暴露。

## 修复

### 1. CpuContext 扩展 GS MSR 字段

```cpp
// process.hpp
struct alignas(16) CpuContext {
    uint64_t r15;       // offset 0
    uint64_t r14;       // offset 8
    uint64_t r13;       // offset 16
    uint64_t r12;       // offset 24
    uint64_t rbp;       // offset 32
    uint64_t rbx;       // offset 40
    uint64_t rsp;       // offset 48
    uint64_t rip;       // offset 56
    uint64_t gs_base;   // offset 64  — MSR_GS_BASE
    uint64_t kgs_base;  // offset 72  — MSR_KERNEL_GS_BASE
};
// sizeof = 80 bytes
```

### 2. context_switch.S 保存/恢复 GS MSR

```asm
# Save
movq $0xC0000101, %rcx       # MSR_GS_BASE
rdmsr
movl %eax, 64(%rdi)          # gs_base low 32
movl %edx, 68(%rdi)          # gs_base high 32

movq $0xC0000102, %rcx       # MSR_KERNEL_GS_BASE
rdmsr
movl %eax, 72(%rdi)          # kgs_base low 32
movl %edx, 76(%rdi)          # kgs_base high 32

# Restore
movl 64(%rsi), %eax
movl 68(%rsi), %edx
movq $0xC0000101, %rcx
wrmsr

movl 72(%rsi), %eax
movl 76(%rsi), %edx
movq $0xC0000102, %rcx
wrmsr
```

### 3. fork() 和 TaskBuilder::build() 初始化 GS 状态

子进程的初始 GS 状态必须是"未交换"状态（内核态默认值）：

```cpp
// fork() 中
child->ctx.gs_base  = 0;
child->ctx.kgs_base = g_per_cpu.gs_page_vaddr;

// TaskBuilder::build() 中
task->ctx.gs_base  = 0;
task->ctx.kgs_base = g_per_cpu.gs_page_vaddr;
```

这样调度器首次恢复子进程时，`wrmsr` 将 GS MSR 设为正确值。

### 4. PerCPU 记录数据页地址

```cpp
// per_cpu.hpp
struct PerCPU {
    Task* current;
    uint64_t kernel_stack;
    uint64_t gs_page_vaddr;    // per-CPU 数据页的虚拟地址

    void update_syscall_stack(uint64_t stack_top) {
        kernel_stack = stack_top;
        if (gs_page_vaddr != 0) {
            *reinterpret_cast<volatile uint64_t*>(gs_page_vaddr) = stack_top;
        }
    }
};
```

- `launch_first_user()` 设置 `g_per_cpu.gs_page_vaddr = gs_phys + KERNEL_VMA`
- 调度器每次 context switch 调用 `update_syscall_stack()` 更新 `gs:0`
- `gui_init.cpp` 子进程进用户态前也调一次

### 修改文件清单

| 文件 | 改动 |
|------|------|
| `kernel/proc/process.hpp` | CpuContext 增加 gs_base/kgs_base 字段 |
| `kernel/arch/x86_64/context_switch.S` | rdmsr/wrmsr 保存恢复两个 GS MSR |
| `kernel/proc/process.cpp` | fork() 和 build() 初始化 GS 字段 |
| `kernel/proc/per_cpu.hpp` | PerCPU 增加 gs_page_vaddr + update_syscall_stack() |
| `kernel/proc/scheduler.cpp` | 每次 context switch 调 update_syscall_stack() |
| `kernel/gui/gui_init.cpp` | 子进程进用户态前调 update_syscall_stack() |
| `kernel/arch/x86_64/usermode.cpp` | launch_first_user 记录 gs_page_vaddr |

## 验证

修复后 QEMU 运行：
- Shell 图标点击 → fork → execve("/bin/sh") → 进入用户态 → **不再崩溃**
- demand-page at 0x0 消失
- syscall 正常工作

## 经验教训

1. **swapgs 是成对操作，调度器必须保证配对完整。** 在 cooperative scheduler 中，任务可以在 swapgs 对之间被抢占（例如 syscall 阻塞），调度器必须保存/恢复 GS MSR 以保持配对语义。

2. **MSR 是 CPU 全局状态，不是 per-task 状态。** x86_64 的 MSR 寄存器不会随 CR3 切换或 RSP 切换而改变。如果多个任务对 MSR 有不同的期望（swapgs 的两个方向），调度器必须显式管理。

3. **Double Fault + RSP=0 = 内核栈指针被破坏。** 当看到 Double Fault 且 RSP 为零或接近零，应首先怀疑是栈加载来源错误（GS base、TSS RSP0 等）。

4. **单核内核的 per-CPU 数据也要正确管理。** 即使只有一个 CPU，per-CPU 数据页的地址也必须在 context switch 时保持一致。
