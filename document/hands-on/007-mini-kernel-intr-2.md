# 007-2 Hands-on: ISR Stub 与异常处理 — 触发异常不死机

> 标签：ISR, Interrupt Frame, 汇编, IRETQ, CR2, Page Fault
> 前置：[007-1 GDT 与 IDT](./007-mini-kernel-intr-1.md)

## 导语

上一篇我们把 GDT 和 IDT 的数据结构和初始化都搭好了，但 IDT 里指向的 ISR stub 还没写——也就是说，虽然 CPU 知道该跳到哪里去处理异常，但跳过去之后发现那里什么都没有。这一篇我们要把最后几块拼图补齐：用汇编编写 ISR stub 保存寄存器，用 C 编写异常处理函数打印信息，最后在 main 里用 `int $3` 做一次完整的点火测试。

完成本篇后，我们就能在串口看到完整的寄存器 dump 和异常信息，而且执行不会中断——触发异常不死机，这就是 milestone 007 的最终目标。

---

## 概念精讲

### 中断栈帧是怎么回事？

当 CPU 响应异常时，它会自动把当前的 SS、RSP、RFLAGS、CS、RIP 压入栈中（注意顺序是从高地址到低地址）。对于某些异常（比如 #PF），CPU 还会额外压入一个错误码。然后 CPU 跳转到 IDT 中指定的处理程序地址。

我们的 ISR stub（汇编写的）在 CPU 压栈的基础上，再把所有通用寄存器保存到栈上，加上一个统一的错误码字段（没有硬件错误码的异常就填 0），这样就形成了一个完整的 InterruptFrame 结构体。C 处理函数收到这个结构体的指针，就能读取异常发生时的完整 CPU 状态。

### AT&T 汇编语法速查

我们的 ISR stub 用 GNU AS（AT&T 语法）编写，几个关键区别需要记住：操作数顺序是"源, 目标"（和 Intel 语法相反），寄存器前缀用百分号 `%`，立即数前缀用美元符号 `$`，内存寻址格式是 `offset(base)`。比如 `pushq %rax` 是把 RAX 的值压栈，`pushq $0` 是压入数字 0，`8(%rsp)` 是访问 RSP+8 处的内存。

### 为什么 #BP 能安全测试而 #PF 不行？

#BP 是 Trap 类型的异常，CPU 压入的 RIP 指向触发指令的下一条——也就是说 IRETQ 返回后会继续执行后续代码，不会反复触发。而 #PF 是 Fault 类型的异常，CPU 压入的 RIP 指向触发指令本身——如果不修复页表就 IRETQ 返回，CPU 会重新执行同一条访存指令，再次触发 #PF，形成死循环。所以我们的 `int $3` 测试可以安全返回，但故意触发 #PF 就需要特殊处理（比如修改 frame 里的 RIP 跳过触发指令）。

---

## 动手实现

### Step 1: 编写 ISR_NOERRCODE 宏（处理 #BP）

**目标**：用 AT&T 汇编写一个宏，生成没有硬件错误码的异常的 ISR stub。

**设计思路**：这个宏需要做以下几件事——首先压入伪错误码 0（保持栈帧统一），然后按固定顺序保存所有 15 个通用寄存器（rax, rbx, rcx, rdx, rbp, rsi, rdi, r8-r15），把当前栈指针作为第一个参数传给 C 处理函数（AT&T 调用约定，第一个参数放 RDI），调用 C 处理函数，然后反向恢复所有寄存器，弹出伪错误码，最后用 IRETQ 返回。

**实现约束**：宏需要接受三个参数——stub 名称、向量号、C 处理函数名。宏生成的符号需要用 `.global` 导出（让链接器能看到），用 `.type name, @function` 标注类型。寄存器的 push 顺序必须和 InterruptFrame 结构体中字段的声明顺序完全一致——结构体从上到下是 r15, r14, ..., rax，但压栈是反向的（先 push rax，最后 push r15），因为栈是从高地址往低地址增长的，先压的在栈底（低地址），对应结构体的靠前成员。

**踩坑预警**：对于 #BP 这种没有硬件错误码的异常，CPU 只压入了 SS/RSP/RFLAGS/CS/RIP 五个值（5×8=40 字节）。如果 ISR stub 不手动压一个伪错误码，C 处理函数读到的 `frame->error_code` 实际上是 CPU 压的 RIP——整个结构体全部错位。这是一个极其隐蔽的 bug，因为它不会立刻 crash，而是让你的寄存器 dump 全部显示错误的值。

**验证**：此步没有独立验证，和后续步骤一起验证。

### Step 2: 编写 ISR_ERRCODE 宏（处理 #PF）

**目标**：写一个类似的宏，但用于 CPU 自动压入错误码的异常。

**设计思路**：和 ISR_NOERRCODE 几乎一模一样，唯一区别是不需要手动压伪错误码——CPU 在进入 ISR 之前已经帮你压了。恢复时仍然用 `addq $8, %rsp` 跳过错误码（CPU 压的），然后 IRETQ。

**踩坑预警**：不要混淆两个宏的用途！如果把 #PF（有错误码的异常）错用了 ISR_NOERRCODE 宏，就会多压一个 0，导致栈帧多出 8 字节，所有后续字段的偏移全部错位。反过来如果 #BP 错用了 ISR_ERRCODE 宏，就会缺少伪错误码，同样错位。每个异常是否有错误码是硬件固定的，可以查 Intel SDM Table 6-1。

**验证**：此步没有独立验证。

### Step 3: 实例化两个 ISR Stub

**目标**：用上面两个宏分别生成 isr_bp_stub 和 isr_pf_stub。

**实现约束**：ISR_NOERRCODE 生成 isr_bp_stub，向量号 3，C handler 名 handle_bp。ISR_ERRCODE 生成 isr_pf_stub，向量号 14，C handler 名 handle_pf。这两个 stub 符号需要在 idt.cpp 中用 `extern "C"` 声明，以便 C++ 代码能引用它们的地址。

**验证**：编译通过即可。

### Step 4: 实现 handle_bp 异常处理函数

**目标**：写一个 C 函数，通过 kprintf 打印断点异常信息和寄存器 dump。

**设计思路**：先写一个辅助函数 dump_interrupt_frame，遍历 InterruptFrame 的所有字段并格式化打印。然后 handle_bp 调用这个辅助函数，再额外打印断点地址（frame->rip）和提示信息。

**实现约束**：所有异常处理函数必须用 `extern "C"` 声明——这是因为 interrupts.S 里的 `call handle_bp` 是 C 链接约定，如果用 C++ 的 name mangling 的话链接器会找不到符号。这是内核开发中混合 C/C++/汇编时的常见模式。

**验证**：此步没有独立验证，和最终整合一起验证。

### Step 5: 实现 handle_pf 异常处理函数

**目标**：写一个 C 函数，读取 CR2 寄存器，解析页错误码，打印详细的页错误信息。

**设计思路**：CR2 寄存器是 x86 架构专门为页错误保留的——CPU 在触发 #PF 时会自动把导致缺页的线性地址写入 CR2，所以我们在处理函数里第一时间把它读出来（用内联汇编 `movq %%cr2, %0`）。然后解析 frame->error_code 的各个位：bit 0 区分"页不存在"和"权限冲突"，bit 1 区分读还是写，bit 2 区分内核态还是用户态，bit 3 是保留位冲突，bit 4 是指令缺页。这些信息在调试缺页问题的时候非常关键。

**踩坑预警**：必须在进入 C 处理函数后第一时间读取 CR2——因为 kprintf 内部的任何内存操作都可能触发另一个页错误（虽然不太可能），覆盖 CR2 的值。虽然在这个简单的 mini kernel 里不太会出问题，但养成好习惯总是对的。

**验证**：此步没有独立验证。

### Step 6: 整合到 main.cpp — 用 int $3 点火测试

**目标**：在 mini_kernel_main 中，PMM 初始化完成后触发断点异常，验证整个中断链路。

**设计思路**：用内联汇编 `__asm__ volatile("int $3")` 触发 #BP 异常。如果一切配置正确，执行流程是：int $3 → CPU 查 IDT[3] 得到 isr_bp_stub 地址 → 查 GDT[1] 确认 code64 段有效 → 压栈 SS/RSP/RFLAGS/CS/RIP → 跳转到 isr_bp_stub → stub 保存寄存器 → 调用 handle_bp → 打印寄存器 dump → 返回 stub → 恢复寄存器 → IRETQ → 继续执行 int $3 后面的代码。

**踩坑预警**：别忘了 include 新文件的头文件，也别忘了把新源文件加入 CMakeLists.txt。最容易被忽略的是 CMakeLists.txt——如果只添加了 .cpp 文件但漏了 .S 文件，链接时就会报 undefined reference。

**验证**：完整构建运行后应该看到如下输出：

```
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)

[TEST] Triggering breakpoint exception (int $3)...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80020xxx   CS  = 0x0008
  RFLAGS= 0x0000000000000046
  RSP   = 0xffffffff80025xxx   SS  = 0x0010
  RAX=0x...  RBX=0x...  ...  R15=0x...
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint triggered at RIP=0xffffffff80020xxx
[EXCEPTION] This is a software breakpoint, continuing...
[TEST] Breakpoint test passed! Execution continued after #BP.
```

看到 `[TEST] Breakpoint test passed!` 就说明整个链路通了：GDT → IDT → ISR stub → C handler → IRETQ → 继续执行。

---

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

---

## 调试技巧

**ISR stub 里寄存器保存顺序错乱**

对比 InterruptFrame 结构体的字段顺序和汇编中的 push 顺序。记住：第一个 push 的值在栈顶（最低地址），对应结构体最后一个字段。我们的结构体最后三个字段是 rcx/rbx/rax，所以汇编里先 push rax，再 push rbx，最后 push rcx。如果顺序搞反了，寄存器 dump 里显示的值就会互相串位——比如你以为看到的是 RAX 的值，实际上是 RBX 的。

**用 GDB 验证 InterruptFrame**

```
(gdb) break handle_bp
(gdb) continue
# 触发 #BP 后断在 handle_bp
(gdb) print *frame
# 检查 frame->rip 是否指向 int $3 的下一条指令
(gdb) info registers
# 对比 frame 里的寄存器值和当前寄存器值
```

**handle_pf 触发后死循环**

这是预期行为——#PF 是 Fault 类型，IRETQ 返回后会重新执行触发缺页的指令。如果不修复页表，就会反复触发 #PF。当前 handle_pf 只打印信息不修复页表，所以故意触发 #PF 会导致死循环打印。这在后续加入 VMM 后会改变——handle_pf 会变成缺页处理的核心。

---

## 本章小结

| 组件 | 关键机制 | 说明 |
|------|----------|------|
| ISR_NOERRCODE 宏 | push $0 + push 寄存器 + call handler + pop + addq $8 + iretq | 无错误码异常的 stub |
| ISR_ERRCODE 宏 | push 寄存器 + call handler + pop + addq $8 + iretq | 有错误码异常的 stub |
| handle_bp | dump frame + 打印断点地址 | Trap 类型，IRETQ 后继续执行 |
| handle_pf | 读 CR2 + 解析 error_code + dump | Fault 类型，不修复页表会死循环 |
| 关键寄存器 | CR2, RDI | CR2 存缺页地址，RDI 传 InterruptFrame* |
| 关键指令 | IRETQ, INT $3 | 中断返回（64位）和软件断点 |
| extern "C" | C 链接约定 | 汇编调用 C 函数必须用 C 链接 |

下一章（008_mini_kernel_disk_and_loader）我们会实现 ATA PIO 磁盘驱动和 ELF 加载器，让 mini kernel 能从磁盘加载大内核并跳转执行。届时 IDT 中已经配好的异常处理会继续发挥作用——大内核加载过程中的任何内存问题都能被 #PF 捕获并报告。
