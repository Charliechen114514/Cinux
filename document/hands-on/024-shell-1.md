# 024-shell-1: GDT 重构与 SYSRETQ SS.RPL 修复

## 导语

在 tag 023 中，我们搭建了 SYSCALL/SYSRETQ 的基本通路，成功让用户态程序调用了 sys_write 和 sys_exit。但当我们试图让 shell 程序在 Ring 3 长时间运行并响应键盘输入时，一个隐蔽的硬件行为差异浮出水面——SYSRETQ 返回用户态后，SS 寄存器的 RPL 字段没有被正确设置为 3。这在 PIT 时钟中断触发 IRETQ 返回 Ring 3 时导致了 #GP 崩溃。本章我们要做的是重构 GDT 布局，使其与 Linux 内核兼容，并修复这个 SS.RPL 问题。完成后，shell 程序将能稳定地在 Ring 3 运行，不再因为中断往返而崩溃。

前置知识：tag 023 的 SYSCALL/SYSRETQ 基础设施、GDT 段描述符格式、x86_64 特权级检查机制。

## 概念精讲

### GDT Selector 布局与 SYSCALL/SYSRETQ

SYSCALL 和 SYSRETQ 指令不依赖 IDT，而是通过 STAR MSR（IA32_STAR，地址 0xC0000081）来确定段选择子。STAR 的布局是高 16 位给 SYSRETQ 用，低 16 位给 SYSCALL 用。具体来说，SYSCALL 入口时 CPU 将 CS 设为 STAR[47:32]、SS 设为 STAR[47:32]+8；SYSRETQ 出口时 CPU 将 CS 设为 STAR[63:48]+16、SS 设为 STAR[63:48]+8。

这里有一个关键细节：Intel SDM 说 SYSRETQ 应该把 SS 的 RPL 强制设为 3（因为要返回 Ring 3），但在某些 QEMU 版本中，这个 RPL 并没有被自动附加。结果是 SS 被设成了内核数据段选择子（比如 0x28），而不是用户数据段（0x2B）。问题并不是立刻显现的——SYSRETQ 刚执行完时，SS 里的错误值暂时不影响运行，因为 64 位模式下 SS 的 base 和 limit 都被忽略。但当中断发生时，CPU 把当前 SS 压入中断栈帧，然后 IRETQ 试图用这个 SS 返回用户态，这时候 SS 的 RPL 检查就会触发 #GP。

Wikipedia 的 x86-64 词条明确指出了 Intel 和 AMD 在这个行为上的分歧：Intel 64 硬件上无条件设置 RPL=3，而 AMD64 的 RPL 来自 STAR 寄存器中的值。QEMU 的某些版本在模拟时没有正确复现这个行为，导致我们在测试中踩到了这个坑。

Linux 内核用了一个简单但巧妙的办法来规避这个问题——把 RPL=3 直接编码进 STAR[63:48] 的基值。也就是说，不让硬件来"追加" RPL，而是让加法运算本身就能产出正确的 selector。这种做法在 Intel 和 AMD 上、在真实硬件和 QEMU 上都表现一致。

### RPL 编码技巧

假如 STAR[63:48] 设为 0x20，SYSRETQ 会算出 SS = 0x20 + 8 = 0x28。这时候如果硬件忘记附加 RPL=3，SS 就是 0x28（RPL=0），触发后续中断返回的 #GP。但如果把 STAR[63:48] 设为 0x23（二进制低两位为 11），那 SS = 0x23 + 8 = 0x2B（RPL=3 已包含在结果中），CS = 0x23 + 16 = 0x33（同理）。无论硬件是否额外设置 RPL，selector 值都是对的。

这个技巧对 SYSCALL 入口路径没有影响——SYSCALL 读的是 STAR[47:32]，SYSRETQ 读的是 STAR[63:48]，两者互不干扰。

为了配合这个修改，GDT 布局也需要调整——需要新增一个 User32 Code 段（Idx 4，selector 0x20）和一个 User64 Code 段（Idx 6，selector 0x30），加上 User Data 段（Idx 5，selector 0x28）。这就是 Linux 采用的"GDT 有独立的 32 位和 64 位用户代码段"布局。为什么要区分 User32 和 User64 代码段呢？因为 SYSRETQ 在返回兼容模式（compatibility mode）时使用 User32 Code 段，返回 64 位模式时使用 User64 Code 段——SYSRETQ 的 CS = STAR[63:48]+16 对应 64 位模式。虽然 Cinux 当前只用 64 位模式，但保留这个布局为将来可能的兼容模式支持留出了空间。

### 旧的 vs 新的 GDT 布局对比

旧的 7 项布局是这样的：Idx 0 为 NULL，Idx 1 (0x08) 是 Kernel Code，Idx 2 (0x10) 是 Kernel Data，Idx 3 (0x18) 是 User Code，Idx 4 (0x20) 是 User Data，Idx 5-6 (0x28) 是 TSS。这个布局的问题是 User Code 段只有一个，没有区分 32 位和 64 位；而且 STAR 基值 0x20 在 QEMU 上不能正确产出带 RPL=3 的 SS。

新的 9 项布局在 Idx 1 增加了一个 NULL 占位（Linux 用作 TLS 段的 placeholder），把内核段往后挪到 Idx 2 和 3，用户段分为 Idx 4 (User32 Code)、Idx 5 (User Data)、Idx 6 (User64 Code)，TSS 移到 Idx 7-8。所有 selector 常量都变了——GDT_KERNEL_CODE 从 0x08 变成 0x10，GDT_USER_CODE 从 0x1B 变成 0x33，等等。

## 动手实现

### Step 1: 重构 GDT 常量

**目标**: 将 GDT selector 常量从旧的 7 项布局更新为 Linux 兼容的 9 项布局。

**设计思路**: 在 gdt.hpp 中更新所有 selector 常量。关键常量包括：GDT_KERNEL_CODE = 0x10（Idx 2，RPL=0），GDT_KERNEL_DATA = 0x18（Idx 3，RPL=0），GDT_USER_CODE = 0x33（Idx 6，RPL=3 已编码），GDT_USER_DATA = 0x2B（Idx 5，RPL=3 已编码），GDT_TSS = 0x38（Idx 7，RPL=0），以及最关键的 GDT_SYSRET_BASE = 0x23。同时将 kEntryCount 从 7 增加到 9。

**踩坑预警**: 改完常量后，所有引用旧 selector 值的地方都会被编译器自动更新——这就是使用常量而非硬编码魔术数的好处。但如果你在其他文件中硬编码了 0x08 或 0x1B 之类的值，那些地方不会自动更新，需要手动查找和替换。另外，host 端的单元测试中也引用了这些常量，需要确保它们与内核头文件保持一致。

**验证**: 修改完成后编译项目，确保所有引用 GDT 常量的地方自动更新。运行 host 端的 GDT/IDT 单元测试，确认 selector 常量测试通过。

### Step 2: 更新 GDT 初始化代码

**目标**: 在 GDT::init() 中填充 9 项描述符，按照新布局排列。

**设计思路**: 每个描述符的 access byte 和 flags 需要精确匹配——Kernel Code（Idx 2）要有 LongMode flag 和 Exec+ReadWrite access，Kernel Data（Idx 3）要有 Size32 flag（数据段不需要 LongMode），User32 Code（Idx 4）要有 Size32（compatibility mode 代码段）和 Ring3 的 DPL，User Data（Idx 5）要有 Size32 和 Ring3 DPL，User64 Code（Idx 6）要有 LongMode 和 Ring3 DPL。TSS 仍然是 16 字节系统段描述符，占据 Idx 7-8，但索引从旧布局的 5 变成了新布局的 7。

注意一个容易忽略的细节：User32 Code 段（Idx 4）的 flags 是 Size32 而不是 LongMode。这是因为 SYSRETQ 在返回兼容模式时会选择 STAR[63:48]+8 对应的段（即 User32 Code），而返回 64 位模式时选择 STAR[63:48]+16（即 User64 Code）。虽然 Cinux 只用 64 位模式，但这个段的 flags 必须正确设置，否则描述符缓存会加载错误的属性。

**验证**: 编译后运行测试。可以在 GDT 初始化后用 sgdt 指令读取 GDTR，然后逐项 dump 描述符的 raw bytes，确认 access byte 符合预期。特别是 GDT[5] 的 access 应该是 0xF2（Present + DPL3 + CodeData + ReadWrite），GDT[6] 应该是 0xFA（Present + DPL3 + CodeData + Exec + ReadWrite）。

### Step 3: 更新 STAR MSR 配置

**目标**: 将 STAR[63:48] 从 0x08 改为 0x23，STAR[47:32] 从 0x08 改为 0x10。

**设计思路**: STAR MSR 的写入使用 wrmsr 指令，格式是 EDX:EAX。EDX 的高 16 位对应 STAR[63:48]（SYSRETQ 基值），EDX 的低 16 位对应 STAR[47:32]（SYSCALL 基值）。值应该是 (0x23 << 16) | 0x10 = 0x00230010。这个修改需要在两个地方同步进行：usermode.S 的 usermode_init_asm 中（用立即数加载），以及 syscall.cpp 的 syscall_init 中（用常量计算）。

**踩坑预警**: 如果 usermode.S 和 syscall.cpp 的 STAR 值不一致，那 usermode_init 和 syscall_init 会互相覆盖，导致行为混乱。usermode_init 先运行，设置一次 STAR；syscall_init 后运行，会再次写入 STAR。确保两处使用相同的值——最安全的做法是在两处都引用 GDT_SYSRET_BASE 和 GDT_KERNEL_CODE 常量。

**验证**: 在 syscall_init 中插入 rdmsr 回读 STAR，打印分解后的 [63:48] 和 [47:32] 值。预期：STAR[63:48] = 0x23，STAR[47:32] = 0x10。也可以在 usermode_init_asm 中用类似的诊断方法验证。

### Step 4: 修复 syscall_init 接口

**目标**: syscall_init() 不再需要外部传入 kernel_rsp 参数，改为内部获取，并在初始化时自动注册所有 builtin syscall handlers。

**设计思路**: 在函数内部用内联汇编 `movq %%rsp, %0` 获取当前 RSP 作为 syscall 入口的内核栈。同时把之前在 main.cpp 中手动注册的 sys_write/sys_exit/sys_yield 调用移到 syscall_init 内部的 register_builtin_handlers() 辅助函数中。新增的 sys_read 也要在这里注册（SYS_read = 0）。这样做的好处是 main.cpp 更简洁，而且新增 syscall handler 时只需改一处。

**验证**: 删除 main.cpp 中手动注册 handlers 的代码后，syscall 仍然能正常 dispatch。运行 syscall 测试确认 LSTAR 和 STAR 都被正确设置。

## 构建与运行

构建命令：

```
cmake --build build
```

运行 QEMU：

```
qemu-system-x86_64 -cdrom build/cinux.iso -serial stdio -display none
```

预期输出：内核启动序列正常，所有 syscall 测试 PASS，用户态程序正常进入 Ring 3。看到 `[SYSCALL] LSTAR=... STAR configured SFMASK=0x200 (clear IF)` 的日志。

## 调试技巧

1. **SYSRETQ 后 SS 值检查**: 在 PIT handler 中打印 InterruptFrame 的 CS 和 SS。如果看到 `SS=0x0028`（RPL=0），说明修复没生效。正常的用户态中断应该是 CS=0x0033、SS=0x002B。
2. **#GP 错误码分析**: IRETQ 的 #GP 错误码就是 CPU 试图加载的那个段 selector。错误码 0x28 指向 User Data 段（Idx 5），RPL=0，说明 SYSRETQ 没有正确设置 RPL。结合 GDT 布局可以快速定位问题。
3. **addr2line 定位崩溃**: `addr2line -e build/kernel/big/big_kernel -f <RIP>` 能 30 秒定位到是哪个函数的哪条指令。如果崩溃在 irq0_stub 的 iretq，基本可以确定是 SS 问题。
4. **STAR 回读验证**: 在 syscall_init 末尾用 rdmsr 读回 STAR，分解打印。如果 STAR[63:48] 不是 0x23，说明写入有误。

## 本章小结

| 概念 | 要点 |
|------|------|
| GDT 布局 | 9 项 Linux 兼容布局，TLS 占位 + User32/User64 分离 |
| STAR[63:48] | 设为 0x23（RPL=3 编码进基值），避免依赖硬件 RPL 设置 |
| Intel vs AMD | Intel 无条件设 RPL=3，AMD 来自 STAR 值，QEMU 可能两者都不对 |
| syscall_init | 内部获取 RSP，自动注册 builtin handlers |
| IRETQ #GP 排查 | 错误码 = 出问题的 selector，配合 GDT 索引定位 |
