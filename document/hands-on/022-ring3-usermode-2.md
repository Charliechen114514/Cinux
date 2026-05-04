# 022-2 · SYSRET MSR 配置与 Ring 0 到 Ring 3 跳转

## 导语

上一章我们搭好了 TSS/IST 这张安全网，现在要真正搭建"跳板"——从 Ring 0 跳到 Ring 3 的具体机制。x86-64 提供了三种进入 Ring 3 的方式：iret（通用但慢）、sysexit（32 位专用）、以及 sysret（64 位长模式首选）。我们选择 sysret，因为它和 syscall 配对使用，是现代 x86-64 操作系统的标准做法。

本章要做两件事：第一步，写一段汇编函数来配置 STAR 和 EFER MSR——这是 SYSRET 的前置条件；第二步，写 `jump_to_usermode` 汇编函数，按照 SYSRET 的寄存器约定设置好所有参数，然后执行 `sysretq` 进入 Ring 3。最后还需要一个 C++ 层面的初始化函数 `usermode_init()` 来调用汇编代码并打印确认信息。

**知识前置**：需要理解 MSR（Model Specific Register）的概念——x86 提供了一组 64 位寄存器，通过 `rdmsr`/`wrmsr` 指令用 ECX 指定索引来读写。还需要了解 AT&T 汇编语法的基本格式（源操作数在前，目标在后）。

## 概念精讲

### STAR MSR (0xC0000081)

STAR 是 SYSCALL/SYSRET 的核心配置寄存器，64 位宽，布局如下：高 16 位 [63:48] 存放 SYSRET 方向的内核 CS 基选择子，中间 16 位 [47:32] 存放 SYSCALL 方向的内核 CS 基选择子，低 32 位 [31:0] 未使用。SYSRET 执行时，CPU 自动从 STAR[63:48] 计算用户态 CS 和 SS——CS 等于 STAR[63:48] + 16 再 OR 上 RPL=3，SS 等于 (STAR[63:48] + 8) OR 3。如果我们把 STAR[63:48] 设为 0x08（内核代码段），那么 SYSRET 后 CS=0x1B、SS=0x13——正好对应 GDT 中预先设置好的用户代码段和数据段。

这里有一个非常容易踩的坑：`wrmsr` 指令只写 EDX:EAX（低 64 位的低 32 位和高 32 位），不写 RDX 的完整 64 位值。如果你用 `shlq $32` 把值移到 RDX 的 63-32 位，`wrmsr` 只读 EDX（低 32 位），高 32 位直接被丢弃。正确的做法是 `shlq $16` 把值放在 EDX 的 31-16 位，这样 `wrmsr` 才能正确写入 STAR 的高 32 位。

### EFER.SCE (0xC0000080 bit 0)

EFER (Extended Feature Enable Register) 的 bit 0 是 SCE (System Call Extensions)。只有当 SCE=1 时，SYSCALL 和 SYSRET 指令才会被 CPU 识别，否则执行时触发 #UD（Undefined Opcode）。EFER 还有其他重要位（如 LMA 表示长模式激活、NXE 表示 No-Execute Enable），所以不能直接 wrmsr 覆盖整个寄存器——必须先 rdmsr 读取当前值，or 上 SCE 位，再 wrmsr 写回。

### SFMASK MSR (0xC0000084)

SFMASK 控制 SYSCALL 指令执行时 RFLAGS 的哪些位会被清除。我们写入 0x200（IF 位掩码），表示 SYSCALL 时自动清除 IF（关中断）。这个设置只影响 SYSCALL 方向，SYSRET 不受 SFMASK 影响——SYSRET 从 R11 恢复 RFLAGS。

**踩坑预警**：QEMU 不完整模拟 SFMASK 的写入持久化。`wrmsr` 写入 0x200 不会触发 #GP（值是合法的），但 `rdmsr` 读回始终返回 0。在真实硬件上写入应该能正常持久化。因为 SFMASK 只影响 SYSCALL 方向而我们当前只用 SYSRET，所以这个问题不影响功能。如果你发现测试中 SFMASK 的 rdmsr 返回 0，别慌——这是 QEMU 的已知行为。

### SYSRET 寄存器约定

SYSRET 对寄存器的使用有严格约定：RCX 必须存放目标 RIP（SYSRET 后加载到 RIP），R11 必须存放目标 RFLAGS（SYSRET 后加载到 RFLAGS）。RSP 不被 SYSRET 修改，所以内核必须在 SYSRET 之前手动设置好用户栈。SYSRET 后，CPL 变为 3，CS 和 SS 由 STAR 寄存器计算得到，中断是否开启取决于 R11 中 IF 位的状态。

## 动手实现

### Step 1: usermode_init_asm — MSR 配置汇编

**目标**：写一个纯汇编函数，按正确顺序配置 STAR、SFMASK 和 EFER MSR。

**设计思路**：函数无输入无输出，纯粹做 MSR 配置。顺序上，先写 STAR（因为后续的 EFER SCE 启用后 SYSCALL/SYSRET 就生效了，所以 STAR 应该先准备好），然后写 SFMASK，最后启用 EFER.SCE。启用 SCE 放在最后是为了确保在 SYSCALL/SYSRET 真正可用之前，所有相关 MSR 都已经配置好。

**实现约束**：STAR 的写入值是 EDX=0x00080008（通过 shlq $16 + orq $0x08 得到），EAX=0。SFMASK 写入 EDX=0、EAX=0x200。EFER 先 rdmsr 读回，然后 orq $1 设置 SCE 位，再 wrmsr 写回。所有 wrmsr 操作通过 ECX 指定 MSR 索引。

**踩坑预警**：再次强调 shlq 的位数。如果你习惯性地写 `shlq $32`，STAR[63:48] 会变成 0 而不是 0x08，SYSRET 后 CS=0x03（零号描述符 + RPL3），CPU 会 Triple Fault 或 #GP。这个 bug 非常隐蔽——因为它不触发异常，只是静默地选择了错误的段选择子。

**验证**：编译链接后，可以在 `usermode_init()` C++ 函数中打印确认信息。后续的 MSR 测试（test_usermode.cpp）会直接 rdmsr 验证 STAR 和 EFER 的值。

### Step 2: jump_to_usermode — SYSRET 跳转汇编

**目标**：写一个汇编函数，接收三个参数（entry、user_stack、arg），设置 SYSRET 所需的寄存器，然后执行 sysretq。

**设计思路**：按照 System V AMD64 ABI，函数参数通过 RDI、RSI、RDX 传入。我们需要把 RDI（entry）移到 RCX（SYSRET 的 RIP 来源），把 RSI（user_stack）移到 RSP（用户栈），把 RDX（arg）移到 RDI（用户程序的第一个参数）。R11 需要设置为 0x202（IF 位 + reserved bit 1），这样 SYSRET 后中断是开启的。其余通用寄存器全部清零，防止内核数据泄露到用户态。

**实现约束**：RFLAGS 值通过 `pushq $0x202; popq %r11` 来设置——比直接 `movq $0x202, %r11` 更简洁。所有通用寄存器（RAX/RBX/RDX/RSI/RBP/R8-R15）都要 xorq 清零。`sysretq` 是 64 位操作数版本的 SYSRET（REX.W 前缀），确保返回 64 位模式而非兼容模式。

**验证**：这个函数调用后不会返回——SYSRET 把控制权交给了 Ring 3 代码。验证方式是检查后续的 Ring 3 代码是否执行（执行特权指令触发 #GP，串口输出异常信息）。

### Step 3: usermode_init() C++ 包装

**目标**：在 C++ 层面提供 `usermode_init()` 函数，调用汇编的 `usermode_init_asm()` 并打印确认日志。

**设计思路**：这是一个简单的包装函数，extern "C" 声明汇编函数，C++ 函数体调用汇编然后 kprintf 打印确认。通过日志可以确认 MSR 配置已执行。

**实现约束**：汇编函数声明为 `extern "C"` 避免 C++ name mangling。usermode_init() 在 kernel_main 的初始化序列中被调用，位于中断使能之后、launch_first_user 之前。

**验证**：运行后串口输出 `[USER] STAR/EFER MSRs configured for SYSRET.`。

## 构建与运行

将汇编文件和 C++ 文件加入 CMakeLists.txt 的编译列表。构建后运行：

```bash
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux.img,format=raw,index=0,media=disk
```

预期看到 `[USER] STAR/EFER MSRs configured for SYSRET.` 日志。如果看到 #UD 异常，说明 EFER.SCE 没有启用或 SYSRET 在非 Ring 0 下执行。

## 调试技巧

1. **shlq 位数错误**：这是本章最隐蔽的 bug。如果 SYSRET 后立刻 Triple Fault 或 #GP，检查 STAR 的 EDX 值。在 usermode_init_asm 中加一段 rdmsr 读回 STAR 并打印 EDX，确认它是 0x00080008 而不是 0x00000008。
2. **#UD after SYSRET**：如果 SYSRET 触发 #UD，说明 EFER.SCE=0。检查 usermode_init_asm 中 EFER 的 rdmsr-wrmsr 序列是否正确执行。
3. **内核数据泄露**：如果 jump_to_usermode 没有清零寄存器，Ring 3 代码可以通过读取寄存器看到内核的 RSP、页表地址等敏感信息。虽然当前我们的用户程序只是 cli;hlt 死循环，但养成清零习惯很重要。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| STAR MSR | [63:48]=SYSRET 内核 CS 基（0x08），[47:32]=SYSCALL 基（0x08） |
| EFER.SCE | bit 0，启用 SYSCALL/SYSRET，必须 rdmsr+or+wrmsr |
| SFMASK | 控制 SYSCALL 时 RFLAGS 屏蔽位，QEMU 不持久化 |
| wrmsr | 只写 EDX:EAX（低 64 位），RDX 高 32 位被忽略 |
| SYSRET 约定 | RCX→RIP, R11→RFLAGS, RSP 需手动设置 |
| RFLAGS | 0x202 = IF(bit 9) + reserved bit 1 |
