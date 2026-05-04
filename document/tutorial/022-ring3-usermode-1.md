# 从 Ring 0 到 Ring 3：x86-64 特权级切换全解析

> 标签：x86-64、Ring 3、SYSRET、特权级、TSS、MSR、页表权限
> 前置：[021 进程同步](021-ring3-usermode.md)

## 前言

说实话，这个 tag 是 Cinux 项目到目前为止最让我兴奋的一个里程碑。前面二十多个 tag，我们一直在 Ring 0（内核态）上运行——所有代码都拥有对硬件的完全控制权，可以随意执行 `cli` 关中断、直接读写 I/O 端口、访问任何内存地址。但一个真正的操作系统不可能让用户程序也跑在 Ring 0，否则恶意软件分分钟把你的系统搞崩。所以，是时候从内核的"舒适区"走出来，进入 Ring 3（用户态）的"受限世界"了。

x86-64 架构定义了四个特权级（Ring 0 到 Ring 3），但实际操作系统中只用两个：Ring 0 给内核，Ring 3 给用户程序。当 CPU 在 Ring 3 运行时，它受三重约束：不能执行特权指令（如 `cli`、`hlt`、`in`、`out`），不能访问标记为 supervisor-only 的内存页，不能使用 Ring 0 的数据段选择子。任何违反都会立刻触发 #GP（General Protection Fault），CPU 自动切换回 Ring 0 执行内核的异常处理函数——这就是特权隔离的核心机制。

不过，从 Ring 0 切换到 Ring 3 可不是一条指令就能搞定的事。我们需要准备好一整套基础设施：GDT 中的用户段描述符（早在 tag 010 就设置好了），TSS 中的 RSP0（Ring 3 异常时 CPU 自动加载的内核栈指针），MSR 寄存器的精确配置（STAR/EFER/SFMASK），以及独立的用户地址空间（带 FLAG_USER 的页表映射）。每一环出了问题，Ring 3 都起不来——不是 Triple Fault 就是 #PF 或者干脆静默挂死。接下来我们就一步步拆解这整个链条。

## 环境说明

和之前一样，我们使用 QEMU 作为模拟器，KVM 或 TCG 加速，`-cpu max` 暴露所有 CPU 特性。编译工具链是 GCC/G++ 13+，CMake 构建系统。需要特别注意的是，QEMU 在 SFMASK MSR 的模拟上有已知缺陷（后文详述），这在真实硬件上不会发生。如果你有条件在物理机上测试，建议对照验证。

## 第一步——TSS 安全网：IST1 和 Double Fault 栈

在我们把 CPU 推到 Ring 3 之前，先得确保"安全网"是到位的——万一 Ring 3 的代码触发了异常，CPU 需要知道去哪里找内核栈。这就是 TSS (Task State Segment) 的职责。在 64 位长模式下，TSS 不再用于硬件任务切换（那个功能被废弃了），而是存储 RSP0/RSP1/RSP2（特权级提升时的栈指针）和 IST1-IST7（中断栈表，为特定中断提供独立栈）。

我们先给 TSS 加上 IST1 支持，专门为 Double Fault (#DF) 服务。Double Fault 是一种"二次异常"——当正常的异常处理本身就失败时触发，最常见的原因是内核栈溢出。如果 Double Fault 还用已经溢出的内核栈来处理，那基本上就是火上浇油。所以我们在 GDT 类中静态分配了一块 4KB 的栈空间，让 TSS 的 ist[0]（IST1）指向这块栈的顶部：

```cpp
// gdt.hpp — Double Fault 栈声明
static constexpr uint64_t DF_STACK_PAGES = 1;
alignas(16) uint8_t df_stack_[DF_STACK_PAGES * 4096]{};

// gdt.cpp — IST1 初始化
tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);
```

然后在 IDT 路由表中，我们把 #DF 的 IST 编号设为 1。原来的路由结构只有四个字段（vector、stub、privilege、gate_type），现在加了第五个 `ist` 字段：

```cpp
{ExceptionVector::DF, isr_df_stub, IDTPrivilege::Kernel,
 IDTGateType::Interrupt, 1},  // <-- IST1
```

其余所有异常保持 IST=0（使用当前 RSP）。这个分配和 Linux 的做法一致，不过 Linux 还给 #NMI 和 #MC 也分配了 IST 栈——对教学内核来说，只有 #DF 使用 IST 已经足够了。

我们同时改造了 #GP 处理函数，让它能区分中断来自 Ring 0 还是 Ring 3：

```cpp
bool from_user = (frame->cs & 0x03) != 0;
if (from_user) {
    kprintf("[EXCEPTION] #GP at RIP=%p from user mode (Ring 3)\n", ...);
} else {
    kprintf("[FATAL] General Protection Fault in kernel mode\n", ...);
}
```

这里检查 InterruptFrame 中 CS 的低两位——Ring 0 的 CS=0x08（低两位=0），Ring 3 的 CS=0x1B（低两位=3）。这是 x86-64 上判断当前特权级的标准方式，Linux 内核的 `user_mode(regs)` 宏做的完全是同一件事。

### 设计对比：Cinux vs xv6 vs Linux 的 TSS 使用

这里值得做一个横向对比。xv6 是 32 位 x86，它的 TSS 结构和 64 位完全不同——32 位 TSS 包含所有通用寄存器、段选择子、CR3、EIP、EFLAGS，总共 104 字节但布局完全不一样。xv6 的 TSS 只用了 SS0 和 ESP0（ Ring 0 的栈段和栈指针），没有 IST 的概念——因为 IST 是 64 位长模式才引入的。xv6 在 `switchuvm()` 中切换 TSS 的 ESP0 来实现 per-process 内核栈，比 Cinux 的做法更完整（Cinux 当前用当前 RSP 作为 RSP0，是一个简化）。

Linux 则是生产级的实现：每个 CPU 有自己的 TSS（per-CPU `cpu_entry_area`），RSP0 指向 per-CPU 的内核栈顶。IST 分配更细致——IST1 给 #DF，IST2 给 #NMI，IST3 给 #MC。而且 Linux 的 entry trampoline 使用特殊的 entry 栈（也是 per-CPU 的），进一步隔离了异常处理路径。Cinux 当前是单核、单 TSS、只有 #DF 用 IST，和 Linux 的差距很大，但核心机制是一致的。

## 第二步——MSR 配置：STAR、EFER、SFMASK

接下来是整个 Ring 3 切换中最精确、最容易出错的部分：配置三个 MSR (Model Specific Register)。SYSRET 指令的行为由 STAR MSR 控制，SYSCALL/SYSRET 的启用由 EFER.SCE 控制，SYSCALL 时的 RFLAGS 屏蔽由 SFMASK 控制。我们写了一个纯汇编函数 `usermode_init_asm` 来做这件事。

**STAR MSR (0xC0000081)** 是 SYSRET 的核心配置。它的 [63:48] 存放 SYSRET 方向的内核 CS 基选择子，SYSRET 执行时 CPU 自动计算 CS = STAR[63:48]+16|RPL3 和 SS = (STAR[63:48]+8)|RPL3。我们把 STAR[63:48] 设为 0x08（内核代码段），所以 SYSRET 后 CS=0x1B、SS=0x13——正好对应 GDT 中预先设置好的用户代码段和数据段。

```asm
movq $0x08, %rdx
shlq $16, %rdx           # EDX[31:16] = 0x08 (SYSRET base)
orq  $0x08, %rdx         # EDX[15:0]  = 0x08 (SYSCALL base)
xorq %rax, %rax
movq $0xC0000081, %rcx   # STAR MSR index
wrmsr                      # EDX:EAX -> STAR
```

这里有一个非常容易踩的坑，值得单独拿出来说：`wrmsr` 指令只写 EDX:EAX（低 64 位的低 32 位和高 32 位），不写 RDX 的完整 64 位值。如果你习惯性地写 `shlq $32` 把值移到 RDX 的 bit 63-32，`wrmsr` 只读 EDX（bit 31-0），高 32 位被丢弃。结果 STAR[63:48] = 0 而不是 0x08，SYSRET 计算出 CS=0x03（零号描述符 + RPL3），CPU Triple Fault。正确的移位量是 $16，把值放在 EDX 的 bit 31-16。

**EFER (0xC0000080)** 的 SCE 位（bit 0）启用 SYSCALL/SYSRET 指令。不能直接 wrmsr 覆盖——EFER 还有 LMA 和 NXE 等重要位。必须先 rdmsr 读回当前值，or 上 SCE 位，再 wrmsr 写回。

**SFMASK (0xC0000084)** 控制 SYSCALL 时 RFLAGS 的哪些位被清除。我们写入 0x200（IF 位掩码），表示 SYSCALL 时自动关中断。但这里有一个 QEMU 的坑：QEMU 不持久化 SFMASK 的写入——`wrmsr` 不报错（值是合法的），但 `rdmsr` 读回始终是 0。这在 KVM 和 TCG 两种后端上都能复现。好消息是 SFMASK 只影响 SYSCALL 方向，SYSRET 从 R11 恢复 RFLAGS，所以我们当前的 SYSRET-only 场景完全不受影响。

### 设计对比：Cinux vs Linux 的 MSR 配置

Linux 在 `syscall_init()` 中配置这些 MSR，代码路径比 Cinux 复杂得多。Linux 的 STAR[63:48] 也是 0x08，但 Linux 还设置了 LSTAR MSR（SYSCALL 入口点，指向 entry_SYSCALL_64）和 CSTAR MSR（compat syscall 入口）。Linux 的 SFMASK 设置了更多位（TF/DF/IF 等），确保 SYSCALL 进入内核时这些调试/异常标志被清除。Cinux 只设置了最基本的位——SCE 和 IF mask，LSTAR 要到 tag 023 实现 syscall handler 时才设置。

## 第三步——SYSRET 跳转：jump_to_usermode

MSR 配好之后，真正的 Ring 3 跳转就简单了——按照 SYSRET 的寄存器约定设置好所有参数，然后一条 `sysretq` 就完事。`jump_to_usermode` 函数接收三个参数：entry（用户入口地址）、user_stack（用户栈顶）、arg（传给用户程序的参数）。

SYSRET 的约定是：RCX → RIP（用户态入口），R11 → RFLAGS（恢复标志寄存器），RSP 不被修改（必须手动设置）。所以我们的汇编做四件事：把 entry 移到 RCX，把 user_stack 移到 RSP，把 arg 移到 RDI（用户程序的第一个参数），然后设置 R11=0x202（IF 位 + reserved bit 1，这样 SYSRET 后中断是开启的）。最后清零所有其他通用寄存器防止内核数据泄露到用户态，执行 `sysretq`。

```asm
movq %rdi, %rcx        # entry -> RCX
movq %rsi, %rsp        # user_stack -> RSP
movq %rdx, %rdi        # arg -> RDI (用户程序参数)
pushq $0x202
popq  %r11             # R11 = RFLAGS (IF + bit1)
xorq %rax, %rax        # 清零所有寄存器...
sysretq                 # 进入 Ring 3!
```

`sysretq` 的 `q` 后缀表示 64 位操作数大小（REX.W 前缀），确保返回 64 位模式。执行后，CPL=3，RIP=0x400000，CS=0x1B，SS=0x13，RFLAGS=0x202（中断开启）。CPU 现在运行在 Ring 3 了。

## 收尾

到这里，整个 Ring 3 切换的硬件层面就完成了。TSS 安全网就位，MSR 精确配置，SYSRET 跳转执行。但有一个问题：我们还没有构建用户态运行所需的地址空间——用户代码在哪里？用户栈在哪里？页表怎么设？这些问题将在下一章中解答，同时我们会深入探讨三个让 Ring 3 起不来的致命 bug。

## 参考资料

- Intel SDM: Vol.3A Section 5.1-5.3 — Protection mechanism, privilege levels
- Intel SDM: Vol.2A SYSRET — SYSRET instruction operation, pseudocode
- Intel SDM: Vol.4 — STAR (0xC0000081), EFER (0xC0000080), SFMASK (0xC0000084)
- OSDev Wiki: [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- felixcloutier.com: [SYSRET](https://www.felixcloutier.com/x86/sysret)
- Linux: arch/x86/entry/entry_64.S, kernel/cpu/common.c (syscall_init)
- xv6: proc.c (userinit, switchuvm), vm.c (setupkvm, inituvm)
