---
title: 010-big-kernel-gdt-idt-2 · GDT/IDT 重构
---

# 010-2 Hands-on: 大内核 IDT 与 ISR 汇编跳板——中断描述符表与中断栈帧

## 导语

上一节我们给大内核配好了 GDT——七个描述符全部到位，段寄存器也都刷新到了正确的选择子。但光有 GDT 还不够，CPU 遇到异常（除零、缺页、非法指令）时需要一个"电话簿"来查找对应的处理程序地址——这就是 IDT（Interrupt Descriptor Table）。没有 IDT，或者 IDT 里对应向量的条目是空的，CPU 就会触发 Double Fault，如果 Double Fault 也没人处理，就 Triple Fault，QEMU 直接重启。

本节我们要完成 IDT 的核心基础设施：创建 IDT 的 C++ 类（配合 scoped enum 表达门类型和特权级）、编写 ISR（Interrupt Service Routine）的汇编跳板代码（负责保存/恢复寄存器），然后用数据驱动的路由表将 ISR stub 注册到 IDT 条目中。

完成本节后，IDT 的初始化和 ISR stub 全部就绪，但还需要下一节的 C++ 异常处理函数才能真正处理异常。

前置知识：上一节（010-1）的 GDT 初始化，以及 009 章的 kprintf 串口输出。

---

## 概念精讲

### IDT：CPU 异常的"电话簿"

IDT 最多有 256 个条目（向量号 0-255），前 32 个（0-31）被 Intel 保留给 CPU 异常。64 位模式下每个条目占 16 字节——比 32 位模式的 8 字节翻了一倍，因为处理程序地址从 32 位扩展到了 64 位，需要额外的空间存放高位地址。IDT 条目中最关键的字段有三个：处理程序地址（被拆成三段存放：offset_low 16位 + offset_mid 16位 + offset_high 32位）、代码段选择子（指向 GDT 中的 code segment 描述符，我们的场景固定是 `GDT_KERNEL_CODE = 0x08`）、以及 type_attr 字节（包含 Present 位、DPL 特权级、门类型）。

### Interrupt Gate vs Trap Gate：关不关中断的区别

IDT 里有两种 64 位门类型。Interrupt Gate（类型 0xE）在跳转到处理程序时 CPU 自动清除 RFLAGS.IF 标志——也就是关中断，处理完后 IRETQ 恢复原来的 IF 状态。Trap Gate（类型 0xF）不动 IF，中断保持原来的开关状态。这个区别的设计意图很明确：对于致命异常（除零、保护错误、页错误等），处理过程中不应该被新的中断打断，所以用 Interrupt Gate；对于调试相关的异常（#BP 断点、#DB 调试），它们本身就是在调试流程中触发的，关掉中断反而会让调试器无法正常工作，所以用 Trap Gate。

还有一个设计决策是关于 DPL（Descriptor Privilege Level）的——#BP 和 #DB 的 DPL 设为 3（用户态可触发），其他异常的 DPL 设为 0（只有内核态能触发）。这样设计是因为 `int $3` 是调试器常用的软件断点指令，用户态程序也可能触发它，所以必须允许 Ring 3 的代码通过这个向量进入内核。

### 中断栈帧：CPU 和 ISR stub 的接力

当 CPU 响应异常时，它会自动把当前的 SS、RSP、RFLAGS、CS、RIP 压入栈中——这是硬件自动完成的，不可跳过。对于某些异常（#DF、#TS、#NP、#SS、#GP、#PF），CPU 还会额外压入一个错误码。对于没有硬件错误码的异常（比如 #DE、#BP），栈上就没有这个东西。

问题在于：我们的 C 处理函数需要接收一个统一的 `InterruptFrame*` 参数来读取所有寄存器的值。如果有的异常栈上有错误码、有的没有，结构体就对不齐了。解决方案是在 ISR stub 里做一个"填零"操作——对于没有硬件错误码的异常，stub 自己推一个假的 error code 0，这样所有异常到达 C 处理函数时，栈帧的布局就完全统一了。

---

## 动手实现

### Step 1: 创建 IDT 头文件——枚举、栈帧结构与类声明

**目标**: 创建 `kernel/arch/x86_64/idt.hpp`，定义 `ExceptionVector` scoped enum（向量 0-14 的符号名）、`IDTGateType` 和 `IDTPrivilege` 枚举、`InterruptFrame` 结构体、`IDT` class（含 Entry/Pointer 内部结构体、Handler/Stub 类型别名、set_handler/init/load 方法）。

**设计思路**: `ExceptionVector` 枚举把魔术数字变成有意义的符号名（`ExceptionVector::DE` 代替 `0`，`ExceptionVector::PF` 代替 `14`）。`IDTGateType` 和 `IDTPrivilege` 枚举使得 IDT 条目的配置在类型层面就是正确的。`InterruptFrame` 结构体的字段顺序必须和 ISR 汇编中 push 的顺序完全一致。

**实现约束**:
- `ExceptionVector` 枚举值：DE=0, DB=1, NMI=2, BP=3, OF=4, BR=5, UD=6, NM=7, DF=8, TS=10, NP=11, SS=12, GP=13, PF=14
- `IDTGateType`: `Interrupt = 0x0E`, `Trap = 0x0F`
- `IDTPrivilege`: `Kernel = 0x00`, `User = 0x60`
- `make_idt_attr(priv, gate)` 函数：`return 0x80 | priv | gate`（0x80 是 Present 位）
- `InterruptFrame` 字段顺序（从上到下）：r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax, error_code, rip, cs, rflags, rsp, ss
- IDT Entry 结构体 16 字节：offset_low(16), selector(16), ist(8), type_attr(8), offset_mid(16), offset_high(32), reserved(32)
- `static_assert(sizeof(Entry) == 16)`
- Handler 类型：`using Handler = void (*)(InterruptFrame*)`
- Stub 类型：`using Stub = void (*)()`
- 全局实例：`extern IDT g_idt`

**踩坑预警**:
- `InterruptFrame` 的字段顺序是"最后 push 的在最上面"——RAX 最后 push（最先声明），R15 最先 push（最后声明）。这个顺序和直觉是反过来的，搞反了的话寄存器值全部是错的，而且编译器不会帮你检查。
- IDT 的第一项（向量 0）是有效的，不像 GDT 的第一项必须是 null。千万别初始化时把向量 0 给跳过了。
- `IDTPrivilege::User = 0x60` 是 `3 << 5` 左移到位后的值（DPL 字段在 type_attr 的 bit 5-6），不是 3。

**验证**: 编译通过即可。`make_idt_attr` 的返回值可以用 `static_assert` 验证：`make_idt_attr(User, Trap)` 应该返回 `0xEF`（0x80|0x60|0x0F），`make_idt_attr(Kernel, Interrupt)` 应该返回 `0x8E`（0x80|0x00|0x0E）。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 2: 编写 ISR 汇编跳板——ISR_NOERRCODE 和 ISR_ERRCODE 宏

**目标**: 创建 `kernel/arch/x86_64/interrupts.S`，定义两个 GAS 宏用于生成 ISR stub，然后实例化 15 个异常处理 stub（向量 0-8, 10-14）。

**设计思路**: 两个宏的区别只有一处——`ISR_NOERRCODE` 在保存寄存器之前先 `pushq $0` 填一个假的 error code，`ISR_ERRCODE` 不需要（CPU 已经 push 了真的 error code）。之后两者的流程完全相同：保存 15 个通用寄存器（RAX->R15 的 push 顺序）、把 RSP 传给 RDI（System V ABI 第一个参数）、调用 C handler、恢复 15 个寄存器、跳过 error code（`addq $8, %rsp`）、`iretq`。

**实现约束**:
- 宏参数：`ISR_NOERRCODE name handler` 和 `ISR_ERRCODE name handler`
- 每个 stub 声明为 `.global \name` 和 `.type \name, @function`
- Push 顺序（从先到后）：%rax, %rbx, %rcx, %rdx, %rbp, %rsi, %rdi, %r8, %r9, %r10, %r11, %r12, %r13, %r14, %r15
- Pop 顺序（与 push 相反）：%r15, %r14, ..., %rax
- 调用方式：`movq %rsp, %rdi` 然后 `call \handler`
- 清理栈：`addq $8, %rsp`（跳过 error code）
- 返回：`iretq`
- 15 个 stub 实例化：
  - 无错误码（ISR_NOERRCODE）：isr_de_stub/handle_de, isr_db_stub/handle_db, isr_nmi_stub/handle_nmi, isr_bp_stub/handle_bp, isr_of_stub/handle_of, isr_br_stub/handle_br, isr_ud_stub/handle_ud, isr_nm_stub/handle_nm
  - 有错误码（ISR_ERRCODE）：isr_df_stub/handle_df, isr_ts_stub/handle_ts, isr_np_stub/handle_np, isr_ss_stub/handle_ss, isr_gp_stub/handle_gp, isr_pf_stub/handle_pf

**踩坑预警**:
- AT&T 语法的操作数顺序是 `source, destination`——`pushq %rax` 是把 RAX 压入栈，`movq %rsp, %rdi` 是把 RSP 复制到 RDI。如果你习惯 Intel 语法，这里非常容易搞反。
- `call \handler` 中的 handler 是 C linkage 函数名。C++ 中定义在 `extern "C"` 块里的函数不会被 name mangle，所以汇编里可以直接用函数名调用。如果你的 handler 忘了加 `extern "C"`，链接时找不到符号。
- 向量 9（Coprocessor Segment Overrun）在现代 CPU 上已经不用了，所以我们跳过了它，实例化的 stub 是 0-8 和 10-14 共 15 个。

**验证**: 此步完成后编译应该通过，但还没有可运行的输出（因为 C handler 还没写）。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 3: 实现 IDT 初始化——数据驱动的路由表

**目标**: 创建 `kernel/arch/x86_64/idt.cpp`，声明 ISR stub 和 C handler 的 extern "C" 引用，实现 `IDT::set_handler()`、`IDT::init()` 和 `IDT::load()`。

**设计思路**: `init()` 使用数据驱动的路由表——一个 `Route` 结构体数组，每个元素包含 {vector, stub, privilege, gate_type}。遍历数组，对每个路由调用 `set_handler()` 配置 IDT 条目。最后计算 IDTR 并调用 `load()` 执行 `lidt`。这比手写 15 次 `set_handler()` 调用更清晰，也更容易扩展。

**实现约束**:
- ISR stub 的 extern "C" 声明：15 个 `void isr_xx_stub()` 函数
- C handler 的 extern "C" 声明：15 个 `void handle_xx(InterruptFrame*)` 函数
- `set_handler(vector, stub, selector, type_attr, ist)` 拆分 64 位地址到三个 offset 字段
- 路由表中的门类型策略：#BP 和 #DB 用 Trap Gate + User privilege，其余全部 Interrupt Gate + Kernel privilege
- 所有条目的 selector 固定为 `GDT_KERNEL_CODE`（0x08）
- 所有条目的 IST 固定为 0（不使用 IST）
- IDTR 的 limit = `sizeof(entries_) - 1`（256 个 16 字节 entry = 4096 字节，limit = 4095），base 指向 entries_ 数组
- `load()` 执行 `asm volatile("lidt %[idtr]" : : [idtr] "m"(idtr_) : "memory")`

**踩坑预警**:
- `set_handler` 中地址拆分的位操作必须正确：offset_low = addr & 0xFFFF, offset_mid = (addr >> 16) & 0xFFFF, offset_high = (addr >> 32) & 0xFFFFFFFF。如果位移搞错了，CPU 跳转到错误的地址处理中断，100% triple fault。
- 路由表里向量 9 被跳过了（现代 CPU 不用），所以向量号是 0-8 和 10-14，不是连续的。

**验证**: 编译通过后进入下一节做完整的集成测试。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### 关于 `extern "C"` 块的细节

`idt.cpp` 文件顶部有两个 `extern "C"` 块。第一个声明了 15 个 ISR stub（定义在 `interrupts.S`），第二个声明了 15 个 C handler（定义在 `exception_handlers.cpp`）。使用块语法 `extern "C" { ... }` 比在每个函数前单独写 `extern "C"` 更简洁。

为什么需要 `extern "C"`？因为 C++ 编译器会对函数名做 name mangling（把 `handle_bp` 变成类似 `_Z9handle_bpPN6cinux4arch15InterruptFrameE` 的内部名），而汇编代码中的 `call handle_bp` 使用的是原始的 C 符号名。如果不加 `extern "C"`，链接器会报 "undefined reference to handle_bp" 错误。这个坑在 C++ 内核开发中非常常见——每次新增一个被汇编调用的 C 函数，都必须记得加 `extern "C"`。

### 关于中断栈帧布局的再强调

`InterruptFrame` 结构体的字段顺序需要再强调一次——从上到下必须和汇编里 push 的顺序完全一致。ISR stub 先 push R15（最后被 push 的在最低地址，也就是结构体的第一个字段），然后依次 R14、R13...RAX，然后是 error_code（CPU 压入的或 stub 填零的），最后是 CPU 自动压入的 RIP、CS、RFLAGS、RSP、SS。

一个验证字段顺序的好方法是：在 `int $3` 之前给某个寄存器设置已知的值（比如 `movq $0xDEADBEEF, %rax`），然后看 dump 里 RAX 是不是这个值。如果不是，字段顺序就搞反了。

**验证**: 编译通过后进入下一节做完整的集成测试。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

---

## 构建与运行

```bash
# 完整编译
cmake --build build --target big-kernel-test-image

# 仅编译不运行
cmake --build build --target big_kernel
```

---

## 调试技巧

**问题: lidt 后 triple fault**
用 QEMU monitor 的 `info registers` 查看 IDTR 的值，确认 base 和 limit 正确。limit 应该是 4095（256 * 16 - 1）。

**问题: ISR stub 链接错误（undefined reference）**
检查 `interrupts.S` 中的 stub 名字和 `idt.cpp` 中 `extern "C"` 声明的名字是否完全一致。注意 C++ 的 name mangling——如果忘了 `extern "C"`，链接器会找不到符号。

**问题: ISR_ERRCODE / ISR_NOERRCODE 用反了**
这是非常危险的 bug。如果一个有硬件错误码的异常（比如 #GP）用了 ISR_NOERRCODE，栈上会多出一个 CPU 压入的错误码，IRETQ 时弹出的 RIP 实际上是错误码的值，CPU 跳到随机地址。反过来，如果无错误码的异常（比如 #BP）用了 ISR_ERRCODE，栈上会少 8 字节，ISR stub 保存的 R15 覆盖了 CPU 压入的 RIP。排查方法是查 Intel SDM 的 Table 6-1 确认每个异常是否有 error code。

**问题: IDT 路由表中向量 9 的处理**
向量 9（Coprocessor Segment Overrun）在现代 x86_64 CPU 上已经不再产生。我们选择跳过它——不注册任何 handler，如果（理论上）触发了，CPU 会走到 IDT[9] 的空条目，触发 #GP。这比注册一个无用的 stub 更简洁。路由表中向量号从 8 直接到 10（不是连续的），编码时不要写成连续的 0-13。

```bash
# GDB 调试
gdb build/kernel/big/big_kernel
(gdb) target remote :1234
(gdb) break isr_bp_stub
(gdb) info registers  # 查看 IDTR
```

---

## 本节小结

| 概念 | 要点 |
|------|------|
| IDT (64-bit) | 每条目 16 字节，地址分三段，最多 256 向量 |
| Interrupt Gate (0xE) | CPU 清除 IF，用于致命异常 |
| Trap Gate (0xF) | CPU 保持 IF，用于 #BP/#DB |
| DPL | #BP/#DB = 3（用户可触发），其余 = 0 |
| ISR_NOERRCODE | push $0 填伪 error code |
| ISR_ERRCODE | CPU 已 push error code |
| InterruptFrame | r15->rax + error_code + rip/cs/rflags/rsp/ss |
| 数据驱动路由表 | Route 数组遍历配置 IDT |
| make_idt_attr | 0x80 | priv | gate 组合 type_attr 字节 |
