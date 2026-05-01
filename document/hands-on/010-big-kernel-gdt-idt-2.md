# 010-2 Hands-on: 大内核 IDT + ISR + 异常处理——中断栈帧与寄存器 dump

## 导语

上一节我们给大内核配好了 GDT——七个描述符全部到位，段寄存器也都刷新到了正确的选择子。但光有 GDT 还不够，CPU 遇到异常（除零、缺页、非法指令）时需要一个"电话簿"来查找对应的处理程序地址——这就是 IDT（Interrupt Descriptor Table）。没有 IDT，或者 IDT 里对应向量的条目是空的，CPU 就会触发 Double Fault，如果 Double Fault 也没人处理，就 Triple Fault，QEMU 直接重启。

本节我们要完成四件事：创建 IDT 的 C++ 类（配合 scoped enum 表达门类型和特权级）、编写 ISR（Interrupt Service Routine）的汇编跳板代码（负责保存/恢复寄存器）、实现一组 C++ 异常处理函数（打印寄存器 dump + 判断致命/非致命）、然后把所有东西集成到 `kernel_main` 里用 `int $3` 触发一个断点异常来验证整个链路。

完成本节后，你会看到这样的串口输出：触发 `int $3` → 串口打印完整的寄存器 dump → 程序继续执行打印 "Breakpoint returned, continuing." → 进入 halt 循环。这就是我们的目标：触发异常不死机，能看到 CPU 当时的完整状态，然后程序还能继续执行。

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

### 致命异常 vs 非致命异常

x86 的 CPU 异常大致可以分成两类。一类是"通知型"的——#BP（断点）和 #DB（调试），处理完后程序可以继续执行。另一类是"致命型"的——#DE（除零）、#UD（非法指令）、#GP（保护错误）、#PF（页错误）等，意味着程序状态已经不可恢复。对于致命异常，我们打印完寄存器 dump 之后进入 `cli; hlt` 死循环。#PF 的处理稍微特殊一点——除了打印寄存器 dump，我们还会从 CR2 寄存器读出触发缺页的虚拟地址，然后把错误码的各个 bit 拆解成可读的字符串。

---

## 动手实现

### Step 1: 创建 IDT 头文件——枚举、栈帧结构与类声明

**目标**: 创建 `kernel/arch/x86_64/idt.hpp`，定义 `ExceptionVector` scoped enum（向量 0-14 的符号名）、`IDTGateType` 和 `IDTPrivilege` 枚举、`InterruptFrame` 结构体、`IDT` class（含 Entry/Pointer 内部结构体、Handler/Stub 类型别名、set_handler/init/load 方法）。

**设计思路**: `ExceptionVector` 枚举把魔术数字变成有意义的符号名（`ExceptionVector::DE` 代替 `0`，`ExceptionVector::PF` 代替 `14`）。`IDTGateType` 和 `IDTPrivilege` 枚举使得 IDT 条目的配置在类型层面就是正确的——你不可能意外地把 Interrupt Gate 的值传给一个期望 Trap Gate 的地方。`InterruptFrame` 结构体的字段顺序必须和 ISR 汇编中 push 的顺序完全一致。

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

**设计思路**: 两个宏的区别只有一处——`ISR_NOERRCODE` 在保存寄存器之前先 `pushq $0` 填一个假的 error code，`ISR_ERRCODE` 不需要（CPU 已经 push 了真的 error code）。之后两者的流程完全相同：保存 15 个通用寄存器（RAX→R15 的 push 顺序）、把 RSP 传给 RDI（System V ABI 第一个参数）、调用 C handler、恢复 15 个寄存器、跳过 error code（`addq $8, %rsp`）、`iretq`。

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
- `addq $8, %rsp` 不能用 `popq` 代替——因为 pop 会把值写入一个寄存器，而 error code 不需要保存到任何地方，直接跳过更高效。不过如果你真的用 pop 到一个临时寄存器然后丢弃，也不会出错，只是多了一次无意义的寄存器写入。

**验证**: 此步完成后编译应该通过，但还没有可运行的输出（因为 C handler 还没写）。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 3: 实现异常处理函数——寄存器 dump 与致命/非致命策略

**目标**: 创建 `kernel/arch/x86_64/exception_handlers.cpp`，实现 15 个 C handler 函数（extern "C" linkage），包含 `dump_registers()` 辅助函数和 `fatal_halt()` 死循环。

**设计思路**: 所有 handler 共享 `dump_registers()` 函数来打印 `InterruptFrame` 中的所有寄存器值（RAX-R15 + RIP + CS + RFLAGS + RSP + SS + Error Code）。非致命异常（#BP 和 #DB）打印完后直接返回（ISR stub 中的 iretq 会恢复执行）。致命异常打印完后调用 `fatal_halt()` 进入 `cli; hlt` 死循环。#PF 特殊处理：从 CR2 读出缺页地址，解码 error code 的各个 bit 成可读字符串。

**实现约束**:
- 所有 handler 函数签名：`void handle_xx(InterruptFrame* frame)`，声明在 `extern "C"` 块中
- `dump_registers(frame, name, vector)` 打印异常名、向量号、所有寄存器值（用 `%p` 格式化指针样式输出）
- `fatal_halt()` 标记为 `[[noreturn]]`，内部是 `while(1) { asm volatile("cli; hlt"); }`
- #BP handler：dump + 打印 "Breakpoint at RIP=xxx" + 打印 "Continuing..."，然后返回
- #DB handler：dump + 打印 "Debug exception, continuing..."，然后返回
- #DE/#NMI/#OF/#BR/#UD/#NM handler：dump + 打印 "FATAL: xxx -- halting."，调用 fatal_halt()
- #DF/#TS/#NP/#SS/#GP handler：dump + 打印 error code 值，调用 fatal_halt()
- #PF handler：dump + 从 CR2 读 faulting address + 解码 error code（present/write/user/reserved/fetch）+ 打印全部信息，调用 fatal_halt()
- CR2 读取方式：`asm volatile("movq %%cr2, %0" : "=r"(fault_addr))`

**踩坑预警**:
- `dump_registers` 中的 `kprintf` 用 `%p` 格式化寄存器值时，需要用 `reinterpret_cast<void*>(value)` 转换，因为 `%p` 期望 `void*` 类型。如果直接传 `uint64_t`，在某些编译器配置下可能打印出错误的值（虽然在实践中大部分编译器会把 uint64_t 和 void* 等价处理，但标准不保证这一点）。
- `fatal_halt()` 中的 `cli; hlt` 放在循环里是因为——虽然 `cli` 已经关了中断，`hlt` 理论上永远不会被唤醒（没有中断来唤醒它），但为了防御性地处理可能的 NMI（不可屏蔽中断），放在循环里是更安全的做法。NMI 可以唤醒 hlt 的 CPU，此时循环回来再 cli; hlt 就行。
- #DF（Double Fault，向量 8）的 error code 总是 0，但 CPU 仍然会 push 它。不要因为知道是 0 就在 ISR stub 里用 ISR_NOERRCODE——Double Fault 必须用 ISR_ERRCODE。

**验证**: 编译通过即可。完整验证在 Step 4 的集成测试中进行。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 4: 实现 IDT 初始化——数据驱动的路由表

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
- 路由表里向量 9 被跳过了（现代 CPU 不用），所以向量号是 0-8 和 10-14，不是连续的。如果你不小心按 0-13 连续编号，向量 9 的 stub 会被安装到向量 10 的位置，然后向量 10 的 ISR stub 对应 #TS 但 IDT 指向了 #MP 的 handler——不会崩但行为是错的。

**验证**: 编译通过后进入 Step 5 做完整的集成测试。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 5: 集成到 kernel_main 并触发 int $3 验证

**目标**: 修改 `kernel/main.cpp`，在 `kernel_main()` 中依次初始化串口、GDT、IDT，然后执行 `asm volatile("int $3")` 触发断点异常。

**设计思路**: 初始化顺序很重要：串口先初始化（kprintf_init），然后 GDT（因为 IDT 的 selector 依赖 GDT 中的 code segment），然后 IDT。初始化完成后打印确认信息，然后用 `int $3` 触发 #BP 异常来验证整个中断处理链路——ISR stub → dump registers → return → continue。

**实现约束**:
- 初始化顺序：`kprintf_init()` → `g_gdt.init()` → `g_idt.init()`
- 在每一步之间打印确认信息
- 触发断点：`asm volatile("int $3")`
- 断点后打印 "Breakpoint returned, continuing."
- 最后进入 `while(1) { asm volatile("cli; hlt"); }` 死循环
- **绝对不能** 在断点测试之前执行 `sti`——因为我们没有 IRQ 处理程序，PIT 定时器中断会触发未处理的 IRQ，导致 Double Fault

**踩坑预警**:
- 如果你手滑在 `int $3` 之前写了 `sti`，QEMU 会在几毫秒内收到 PIT 定时器中断（IRQ 0），然后跳到 IDT[32]——一个空条目——触发 #GP → Double Fault → Triple Fault → 重启。这个坑我踩了不止一次。
- `int $3` 是一字节指令（opcode 0xCC），而 `int $N`（N != 3）是两字节指令（0xCD N）。`int $3` 被设计成一字节是有原因的——调试器可以在任意位置插入断点（只需覆盖一个字节），而不需要考虑指令对齐。不过对我们来说这两个形式效果一样。

**验证**: 运行大内核，串口应该输出类似以下内容：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...   CS  = 0x0008
  RFLAGS= 0x...
  RSP   = 0x...   SS  = 0x0010
  ... (all register values)
  ERROR CODE = 0x0
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

```bash
# 运行大内核
cmake --build build --target run 2>&1 | head -40

# 或者运行自动化测试
cmake --build build --target run-big-kernel-test
# 预期：8 个测试全部 PASS，QEMU 自动退出
```

### Step 6: 编写集成测试（可选但推荐）

**目标**: 创建 `kernel/test/test_gdt_idt.cpp`，在 QEMU 内运行自动化测试验证 GDT 段寄存器值和 #BP 异常处理。

**实现约束**:
- 测试框架定义在 `kernel/test/big_kernel_test.h`（轻量级，用 kprintf 输出）
- 测试用例：读 CS/DS/SS/ES 寄存器验证值、触发 int $3 验证程序继续执行、多次触发验证栈帧不损坏、验证 make_idt_attr 的返回值
- 测试入口在 `kernel/test/main_test.cpp`（替代 kernel_main）
- 测试完成后通过 QEMU isa-debug-exit 设备退出

**验证**:

```bash
cmake --build build --target run-big-kernel-test
# 预期输出：
# [TEST] Big Kernel Test Suite starting...
# [TEST] GDT loaded.
# [TEST] IDT loaded.
# === Big Kernel GDT/IDT/Interrupt Tests (010) ===
# [RUN] test_cs_register
# [PASS] test_cs_register
# ... (8 tests)
# === Tests: 8 passed, 0 failed ===
# [TEST] ALL TESTS PASSED
```

---

## 构建与运行

```bash
# 完整编译
cmake --build build --target big-kernel-test-image

# 运行测试（自动退出）
cmake --build build --target run-big-kernel-test

# 运行生产内核（手动 Ctrl+C 退出）
cmake --build build --target run

# 仅编译不运行
cmake --build build --target big_kernel
```

---

## 调试技巧

**问题: int $3 触发后 triple fault**
最可能的原因是 IDT 中向量 3 的条目配置不对。用 QEMU 的 `-d int` 选项可以查看每次中断/异常的详细信息，包括向量号、error code、IDT 条目内容。`-d int` 的输出里会显示 CPU 在 IDT 中找到了什么——如果是 `type=0` 说明条目为空（未 present），如果是 `selector=0x0000` 说明段选择子没设。

**问题: 寄存器值看起来全部是错的**
`InterruptFrame` 的字段顺序和汇编中的 push 顺序不一致。回头检查 idt.hpp 中的结构体定义——r15 必须在最前面（因为它最先被 push，在栈上最深），rax 在最后面（因为它最后被 push，在栈上最浅）。

**问题: 多次 int $3 后程序崩溃**
ISR stub 中的 `addq $8, %rsp` 是否正确？如果漏了这一步，每次 iretq 后栈上会残留 error code，导致栈指针偏移，最终栈溢出或对齐错误。另一个可能是 push/pop 顺序不对称——少 pop 了一个寄存器或者多 push 了一个。

**调试命令**:

```bash
# 查看 QEMU 中断事件日志
qemu-system-x86_64 ... -d int 2>int.log
# 触发异常后查看 int.log

# GDB 调试 ISR
gdb build/kernel/big/big_kernel
(gdb) target remote :1234
(gdb) break isr_bp_stub
(gdb) break handle_bp
(gdb) info registers  # 对比 dump_registers 的输出

# 查看段寄存器
(gdb) info registers cs ds ss es fs gs
# 预期：cs=0x0008, ds=0x0010, ss=0x0010
```

---

## 本章小结

| 概念 | 要点 |
|------|------|
| IDT (64-bit) | 每条目 16 字节，地址分三段，最多 256 向量 |
| Interrupt Gate (0xE) | CPU 清除 IF，用于致命异常 |
| Trap Gate (0xF) | CPU 保持 IF，用于 #BP/#DB |
| DPL | #BP/#DB = 3（用户可触发），其余 = 0 |
| ISR_NOERRCODE | push $0 填伪 error code |
| ISR_ERRCODE | CPU 已 push error code |
| InterruptFrame | r15→rax + error_code + rip/cs/rflags/rsp/ss |
| dump_registers | 格式化输出所有寄存器值 |
| fatal_halt | cli; hlt 死循环，用于致命异常 |
| #PF 特殊处理 | 读 CR2 + 解码 error code bit |
| 数据驱动路由表 | Route 数组遍历配置 IDT |
| 初始化顺序 | kprintf_init → gdt.init → idt.init |
| 不要 sti | 没有 IRQ handler 时开中断会 DF |
