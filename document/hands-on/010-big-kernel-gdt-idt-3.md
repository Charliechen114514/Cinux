# 010-3 Hands-on: 异常处理函数与集成验证——寄存器 dump 与 int $3 测试

## 导语

前两节我们搭好了 IDT 和 ISR 汇编跳板——IDT 的 14 个路由条目全部注册完毕，ISR stub 也实例化好了。但 ISR stub 里的 `call \handler` 调用的 C 函数还没实现。本节我们要做最后两件事：实现 15 个 C++ 异常处理函数（打印寄存器 dump + 判断致命/非致命），然后把所有东西集成到 `kernel_main` 里用 `int $3` 触发一个断点异常来验证整个链路。

完成本节后，你会看到这样的串口输出：触发 `int $3` -> 串口打印完整的寄存器 dump -> 程序继续执行打印 "Breakpoint returned, continuing." -> 进入 halt 循环。这就是我们的目标：触发异常不死机，能看到 CPU 当时的完整状态，然后程序还能继续执行。

前置知识：010-1 的 GDT 初始化、010-2 的 IDT 与 ISR 汇编，以及 009 章的 kprintf 串口输出。

---

## 概念精讲

### 致命异常 vs 非致命异常

x86 的 CPU 异常大致可以分成两类。一类是"通知型"的——#BP（断点）和 #DB（调试），处理完后程序可以继续执行。另一类是"致命型"的——#DE（除零）、#UD（非法指令）、#GP（保护错误）、#PF（页错误）等，意味着程序状态已经不可恢复。对于致命异常，我们打印完寄存器 dump 之后进入 `cli; hlt` 死循环。

#PF 的处理稍微特殊一点——除了打印寄存器 dump，我们还会从 CR2 寄存器读出触发缺页的虚拟地址，然后把错误码的各个 bit 拆解成可读的字符串（是读还是写、是用户态还是内核态、是页不存在还是权限违反）。这些信息对于后续调试内存管理相关的 bug 非常有价值。

### AT&T 汇编语法速查

本节涉及的内联汇编指令速查：

| 操作 | AT&T 语法 | 含义 |
|------|-----------|------|
| 压立即数 | `pushq $0` | 把数字 0 压入栈 |
| 读 CR2 | `movq %%cr2, %0` | 读页错误地址寄存器 |
| 关中断 + 停机 | `cli; hlt` | 关中断后暂停 CPU |
| 触发断点 | `int $3` | 触发 #BP 异常（opcode 0xCC） |

操作数顺序永远是 `source, destination`。

---

## 动手实现

### Step 1: 实现异常处理函数——寄存器 dump 与致命/非致命策略

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
- `dump_registers` 中的 `kprintf` 用 `%p` 格式化寄存器值时，需要用 `reinterpret_cast<void*>(value)` 转换，因为 `%p` 期望 `void*` 类型。
- `fatal_halt()` 中的 `cli; hlt` 放在循环里是因为——虽然 `cli` 已经关了中断，`hlt` 理论上永远不会被唤醒（没有中断来唤醒它），但为了防御性地处理可能的 NMI（不可屏蔽中断），放在循环里是更安全的做法。
- #DF（Double Fault，向量 8）的 error code 总是 0，但 CPU 仍然会 push 它。不要因为知道是 0 就在 ISR stub 里用 ISR_NOERRCODE——Double Fault 必须用 ISR_ERRCODE。

**验证**: 编译通过即可。完整验证在 Step 2 的集成测试中进行。

```bash
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 2: 集成到 kernel_main 并触发 int $3 验证

**目标**: 修改 `kernel/main.cpp`，在 `kernel_main()` 中依次初始化串口、GDT、IDT，然后执行 `asm volatile("int $3")` 触发断点异常。

**设计思路**: 初始化顺序很重要：串口先初始化（kprintf_init），然后 GDT（因为 IDT 的 selector 依赖 GDT 中的 code segment），然后 IDT。初始化完成后打印确认信息，然后用 `int $3` 触发 #BP 异常来验证整个中断处理链路——ISR stub -> dump registers -> return -> continue。

**实现约束**:
- 初始化顺序：`kprintf_init()` -> `g_gdt.init()` -> `g_idt.init()`
- 在每一步之间打印确认信息
- 触发断点：`asm volatile("int $3")`
- 断点后打印 "Breakpoint returned, continuing."
- 最后进入 `while(1) { asm volatile("cli; hlt"); }` 死循环
- **绝对不能** 在断点测试之前执行 `sti`——因为我们没有 IRQ 处理程序，PIT 定时器中断会触发未处理的 IRQ，导致 Double Fault

**踩坑预警**:
- 如果你手滑在 `int $3` 之前写了 `sti`，QEMU 会在几毫秒内收到 PIT 定时器中断（IRQ 0），然后跳到 IDT[32]——一个空条目——触发 #GP -> Double Fault -> Triple Fault -> 重启。
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

### Step 3: 编写集成测试（可选但推荐）

**注意**: 本步骤是可选的。如果你只验证生产内核的 `int $3` 输出，可以跳过这一步直接看 Step 2 的串口输出。但如果你希望有自动化的 PASS/FAIL 判断（而不是手动看串口输出），建议完成本步骤。

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
最可能的原因是 IDT 中向量 3 的条目配置不对。用 QEMU 的 `-d int` 选项可以查看每次中断/异常的详细信息，包括向量号、error code、IDT 条目内容。

```bash
qemu-system-x86_64 ... -d int 2>int.log
# 触发异常后查看 int.log
```

**问题: 寄存器值看起来全部是错的**
`InterruptFrame` 的字段顺序和汇编中的 push 顺序不一致。在 `int $3` 之前给某个寄存器设置已知的值（比如 `movq $0xDEADBEEF, %rax`），然后看 dump 里 RAX 是不是这个值。

**问题: 多次 int $3 后程序崩溃**
ISR stub 中的 `addq $8, %rsp` 是否正确？如果漏了这一步，每次 iretq 后栈上会残留 error code，导致栈指针偏移。

```bash
# GDB 调试 ISR
gdb build/kernel/big/big_kernel
(gdb) target remote :1234
(gdb) break isr_bp_stub
(gdb) break handle_bp
(gdb) info registers  # 对比 dump_registers 的输出
```

---

## 本章小结

| 概念 | 要点 |
|------|------|
| dump_registers | 格式化输出所有寄存器值到串口 |
| fatal_halt | cli; hlt 死循环，用于致命异常 |
| #BP handler | 非致命，dump 后返回继续执行 |
| #PF handler | 读 CR2 + 解码 error code bit，然后 halt |
| 初始化顺序 | kprintf_init -> gdt.init -> idt.init |
| 不要 sti | 没有 IRQ handler 时开中断会 DF |
| int $3 | 一字节断点指令（0xCC），触发 #BP |

---

## 完整 tag 010 回顾

本 tag（010_big_kernel_gdt_idt）通过三篇文章覆盖了大内核异常处理基础设施的完整搭建过程：

| 文章 | 主题 | 涉及文件 |
|------|------|----------|
| 010-1 | GDT 初始化 | gdt.hpp, gdt.cpp |
| 010-2 | IDT + ISR 汇编跳板 | idt.hpp, idt.cpp, interrupts.S |
| 010-3 | 异常处理函数 + 集成验证 | exception_handlers.cpp, main.cpp, test_gdt_idt.cpp |

整条链路的依赖关系是：GDT -> IDT -> ISR stub -> C handler -> dump_registers + fatal_halt。初始化顺序必须严格遵守 kprintf_init -> g_gdt.init -> g_idt.init，因为每一步都依赖前一步的产出（IDT 依赖 GDT 中的段选择子，handler 的 kprintf 依赖串口初始化）。
