---
title: 022-ring3-usermode-1 · 用户态 (Ring 3)
---

# 022-1 · TSS/IST 初始化与特权级基础设施

## 导语

到上一个 tag 为止，我们的内核一直在 Ring 0（最高特权级）上运行，所有代码都拥有对硬件的完全控制权。但一个真正的操作系统不可能让用户程序也跑在 Ring 0——那样的话，任何用户代码都能直接执行 `cli` 关中断、直接读写 I/O 端口、直接访问内核数据结构，安全根本无从谈起。所以这一章我们开始搭建从 Ring 0 到 Ring 3（用户态）切换所需的基础设施。

具体来说，本章我们要做三件事：给 GDT 中的 TSS 补上 IST1（中断栈表）支持，让 IDT 中的 Double Fault 中断使用独立的 IST1 栈，以及修改 #GP 处理函数使其能区分中断来自内核态还是用户态。完成这些之后，我们就拥有了进入 Ring 3 的"安全网"——即使 Ring 3 的用户程序触发了异常，CPU 也能正确地切换到内核栈来处理。

**知识前置**：上一章 (021_proc_sync) 我们已经有了 GDT、IDT、TSS 的基本结构。如果你跳过了前面的 tag，至少需要确认 GDT 中已经有 TSS 描述符（type=0x89），并且 `ltr` 指令已经执行过加载 TSS 选择子。

**完成本章后**：你将看到 GDT 初始化日志中多出 "TSS with IST1 Double Fault stack" 的字样，IDT 日志中显示 "#DF uses IST1"。虽然这些改动本身不会直接进入 Ring 3，但它们是 Ring 3 异常处理路径的关键组成部分。没有 IST1，Double Fault 会用已经溢出的内核栈处理，导致 Triple Fault。没有 #GP 用户态检测，我们无法确认特权隔离是否真正生效。

**本章涉及的关键文件**：`kernel/arch/x86_64/gdt.hpp`（TSS 结构 + Double Fault 栈声明）、`kernel/arch/x86_64/gdt.cpp`（IST1 初始化）、`kernel/arch/x86_64/idt.cpp`（路由表扩展 + IST 字段）、`kernel/arch/x86_64/exception_handlers.cpp`（#GP 用户态检测）。

**本章修改摘要**：修改 4 个文件，新增约 30 行代码。核心改动——GDT 增加 IST1 Double Fault 栈、IDT 路由表增加 IST 字段、#GP handler 增加用户态检测。

## 概念精讲

### 特权级 (Ring 0 / Ring 3)

x86-64 定义了四个特权级（Ring 0 到 Ring 3），但实践中只用 Ring 0（内核）和 Ring 3（用户）。当前特权级由 CS 寄存器低两位（CPL 字段）决定。Ring 3 的代码不能执行特权指令（如 `cli`、`hlt`、`in`、`out`），不能访问标记为 supervisor-only 的内存页，不能直接操作控制寄存器。一旦违反这些约束，CPU 立刻触发 #GP（General Protection Fault），把控制权交还给内核——这就是特权隔离的核心机制。

这里有一个容易混淆的地方：特权级的数字越大权限越低。Ring 0 是"上帝模式"——什么都能做，Ring 3 是"受限模式"——只能做操作系统允许的事。你可能会问，为什么不用 Ring 1 和 Ring 2 呢？因为 x86 的历史包袱——这两个特权级确实存在于硬件中，但主流操作系统（Linux、Windows、macOS）都不使用它们，所以你可以把 x86-64 的特权模型简化为"Ring 0 vs Ring 3"二选一。

Intel SDM Vol.3A Section 5.1 对特权级机制有完整的描述，其中最关键的一点是：特权级检查发生在三个地方——代码段切换（CS 加载）、数据段访问（DS/ES/FS/GS/SS 加载）、以及页表访问（U/S 位）。这三个检查共同构成了 x86-64 的特权隔离体系。

**特权级检查实例**：当你执行 `cli` 指令（清除中断标志）时，CPU 首先检查 CPL 是否为 0。如果 CPL=3，CPU 不执行指令，而是触发 #GP（vector 13）。类似地，当你尝试通过 `mov cr0, rax` 修改控制寄存器时，CPL 也必须为 0。I/O 端口访问（`in`/`out`）的权限稍复杂——它取决于 EFLAGS.IOPL 和 TSS 中的 I/O Permission Bitmap，但 Ring 3 默认不能访问 I/O 端口。

**页表层面的特权检查**：每一级页表条目都有一个 U/S 位（bit 2）。U/S=0 表示 supervisor-only（只有 Ring 0 可访问），U/S=1 表示 user（Ring 0 和 Ring 3 都可访问）。当 Ring 3 的代码访问一个 U/S=0 的页时，CPU 触发 #PF（Page Fault）而不是 #GP——这是很多初学者容易混淆的地方。#GP 是"指令/段权限不够"，#PF 是"页表权限不够"。

### TSS 在 Long Mode 下的角色

在 32 位保护模式下，TSS 确实用于硬件任务切换，每个任务有自己的 TSS，CPU 可以通过 TSS 自动保存和恢复寄存器状态。但到了 64 位长模式，硬件任务切换被废弃了——Intel 在 64 位模式下不再支持 `call` 或 `jmp` 到 TSS 描述符的操作。TSS 的角色变成了纯粹的"栈切换表"。它不再保存通用寄存器，只保留几个关键字段：RSP0/RSP1/RSP2（特权级提升时加载的栈指针）和 IST1 到 IST7（中断栈表，为特定中断提供独立栈）。

整个 Long Mode TSS 只有 104 字节，结构非常紧凑。OSDev Wiki 的 [Task State Segment](https://wiki.osdev.org/Task_State_Segment) 页面有完整的字段布局表，其中最关键的几个偏移量需要记住：RSP0 在 offset 0x04（注意不是 0x00——0x00 是 reserved），IST1 在 offset 0x24，IOPB（I/O Permission Bitmap 偏移量）在 offset 0x64。我们不使用 I/O 权限位图时通常把 IOPB 设为 sizeof(TSS) = 104，表示位图为空。

当 Ring 3 的代码触发中断或异常时，CPU 自动从 TSS.RSP0 加载内核栈指针——这就是为什么进入 Ring 3 之前必须设置好 TSS.RSP0。如果 RSP0 没设好或者指向了无效地址，你会收获一个非常漂亮的 Triple Fault，因为 CPU 连异常处理函数的栈帧都无法正确构造，更别提执行处理函数了。

**RSP0 的设置时机**：RSP0 必须在每次切换到 Ring 3 之前设置，因为不同的用户进程可能需要不同的内核栈。在本章中我们先用一个简化做法——把当前内核 RSP 存入 RSP0。后续多进程支持时，每个进程会有独立的内核栈，RSP0 在上下文切换时更新。Linux 的做法是每个 task_struct 中保存一个内核栈指针，switch_to 时更新 RSP0。

### IST (Interrupt Stack Table)

IST 是 TSS 中的一组额外栈指针（IST1-IST7），专门用于处理"内核栈本身可能已经不可靠"的异常场景。最典型的例子就是 Double Fault (#DF)：当内核栈溢出导致正常异常处理失败时，CPU 会触发 Double Fault。如果 Double Fault 还用内核栈来处理，那基本上就是火上浇油——栈已经溢出了，你还要在溢出的栈上处理异常？

所以我们在 IDT 中给 #DF 指定 IST1，让 CPU 切换到一个预先分配好的、独立的 4KB 栈来处理 Double Fault。IST 的设计思路可以用一句话概括：把鸡蛋放在不同的篮子里。内核栈是一个篮子，IST1 栈是另一个篮子。即使内核栈这个篮子翻了（溢出），IST1 这个篮子还是好的，可以安全地处理 Double Fault。

IST 编号存储在 IDT 门描述符的 byte 4（偏移量 4）的低 3 位中。当 CPU 处理中断时，如果 IDT 条目的 IST 字段非零，CPU 就从 TSS 的对应 IST 字段（IST1-IST7）加载 RSP，而不是从 RSP0/RSP1/RSP2 加载。这个选择发生在中断帧构造之前，所以 IST 栈切换是原子性的——不会出现"切换了一半"的中间状态。

**IST 与普通 RSP 切换的区别**：普通的特权级切换（如 Ring 3 → Ring 0）使用 TSS.RSP0 作为新栈——这个值是 per-CPL 的，RSP0 对应 CPL=0 的栈。IST 则是完全独立的机制——它不看 CPL，只看 IDT 条目中的 IST 编号。这意味着 IST 既可以用于 Ring 0 内部的栈切换（如 Double Fault，此时 CPL 不变），也可以用于跨特权级的栈切换（如 Ring 3 的 NMI）。

**IST 的局限性**：IST 栈不会自动重置——每次使用后必须手动恢复 IST 指针到栈顶。这是因为 IST 只是一个指向固定地址的指针，CPU 从 IST 加载 RSP 后会在该栈上 push 数据，但不会在处理完后自动恢复 IST 值。如果在 IST 栈处理期间嵌套了另一个使用同一 IST 的中断，第二次 push 会覆盖第一次的数据。这就是为什么 Linux 只给极少数关键异常分配 IST——Double Fault、NMI、Machine Check，这些异常通常不会嵌套。

## 动手实现

**GDT 当前布局回顾**：在动手之前，确认一下当前 GDT 的 slot 分配。slot 0 是 null，slot 1 (0x08) 保留给 TLS placeholder，slot 2 (0x10) 是内核代码段，slot 3 (0x18) 是内核数据段，slot 4 (0x20) 是 User32 代码段，slot 5 (0x28) 是用户数据段，slot 6 (0x30) 是 User64 代码段，slot 7-8 (0x38) 是 TSS 描述符（64 位 TSS 占 16 字节 = 2 个 slot）。GDT_KERNEL_CODE=0x10，GDT_KERNEL_DATA=0x18，GDT_USER_CODE=0x33（0x30|3），GDT_USER_DATA=0x2B（0x28|3），GDT_TSS=0x38。这些段描述符在 tag 010 就设置好了，本章只需要修改 TSS 内容（加上 IST1），不需要修改 GDT 的段选择子。

### Step 1: 给 TSS 添加 IST1 Double Fault 栈

**目标**：在 GDT 类中静态分配一块 4KB 对齐的栈空间，让 TSS 的 IST1 字段指向这块栈的顶部。

**设计思路**：Double Fault 栈不需要很大——它只在最严重的错误场景下使用，处理函数通常只是打印一条消息然后停机。4KB 足够了。关键是这块栈必须独立于内核栈，否则当内核栈溢出时 IST 栈也跟着完蛋。我们把这块栈作为 GDT 类的成员，编译时就分配好，不存在运行时分配失败的风险。这种做法虽然占用了 4KB 的 BSS 空间，但消除了动态分配可能失败的问题——对于 Double Fault 这种最严重的异常场景，可靠性比灵活性重要得多。

**实现约束**：你需要做以下几处修改。首先，在 GDT 类的 private 区域添加两个新成员：一个 `uint64_t df_stack_phys_` 用于记录物理地址（当前未使用但预留），以及一个 `alignas(16) uint8_t df_stack_[DF_STACK_PAGES * 4096]` 数组作为实际的栈缓冲区（DF_STACK_PAGES = 1）。栈缓冲区必须 16 字节对齐（x86-64 ABI 要求栈指针在调用任何函数前必须是 16 字节对齐的）。然后在 GDT::init() 函数中，在 TSS 描述符写入 GDT 之前，把 TSS 的 ist[0]（即 IST1）设为 `reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)])`。注意这里用的是数组末尾地址而不是起始地址——因为栈是从高地址向低地址增长的。

**为什么要用 DF_STACK_PAGES 常量而不是硬编码 4096**：使用命名常量有几个好处：首先，代码意图更清晰——读者一看就知道这是"几页"而不是"多少字节"。其次，如果将来需要增大 Double Fault 栈（比如 Double Fault handler 需要调用更复杂的函数），只需要改常量值。最后，常量可以用于 sizeof 计算，减少硬编码数字。

**踩坑预警**：TSS 结构必须用 `[[gnu::packed]]` 修饰，否则编译器可能插入填充字节导致结构大小不等于 104 字节。如果 static_assert 报错说 TSS 不是 104 字节，检查是不是漏了 packed 属性或者字段类型写错了（比如把 uint64_t 写成了 uint32_t——这在 TSS 结构中特别容易发生，因为 reserved0 和 reserved3 是 32 位的而其余大多是 64 位的）。

**验证**：编译通过后，在 GDT init 的日志中确认 IST1 被设置了。可以通过在 init 中打印 TSS.ist[0] 的值来确认它非零且等于 df_stack_ 数组末尾的地址。如果值为 0，说明 IST1 初始化代码的位置不对——它必须在 TSS 描述符写入 GDT 之前执行，因为 ltr 之后 CPU 就可能通过 TSS 访问 IST 字段了。

**测试覆盖**：Host 端单元测试中有一个 TestTSS 镜像结构，验证 TSS 大小为 104 字节、rsp 数组有 3 个条目、ist 数组有 7 个条目、以及各字段的偏移量。内核测试中通过 `str` 指令验证 TR 已加载为 GDT_TSS 选择子。这些测试确保 TSS 布局不会因意外的 struct 修改而悄悄出错。

### Step 2: 修改 IDT 路由表，给 #DF 分配 IST1

**目标**：在 IDT 的异常路由表中增加 IST 字段，将 #DF 的 IST 设为 1，其余异常保持 0。

**设计思路**：原来 IDT 路由结构只有四个字段（vector、stub、privilege、gate_type），现在加第五个 `ist` 字段（uint8_t）。`set_handler` 函数在构造 IDT 条目时需要把这个 ist 值写入条目的 IST 偏移位。所有异常的 IST 都设为 0，只有 #DF 设为 1。这和 Linux 的做法一致——Linux 还给 #NMI 分配 IST2，给 #MC (Machine Check) 分配 IST3，但 Cinux 目前只需要 #DF 使用 IST。你需要在 Route 结构定义中加上 ist 字段，然后在路由数组中给 #DF (Double Fault) 那一行添加 `, 1`，其余行添加 `, 0`。最后修改 set_handler 的调用，把 r.ist 作为第五个参数传入。

**实现约束**：IDT 条目的 IST 编号存储在中断门描述符的 byte 4（偏移量 4）的 bit 0-2。0 表示不使用 IST（使用当前 RSP 或 TSS.RSPn），1-7 表示使用对应的 IST 栈。IST 值必须在 0-7 范围内，超出范围的值会导致 CPU 行为未定义。set_handler 函数需要把这个 ist 值正确地写入 IDT 条目的对应位置。

**数据驱动路由表的好处**：我们使用 Route 结构数组而不是逐个手写 set_handler 调用，好处是新增中断时只需加一行——而且不容易遗漏 IST 字段。如果你用分散的 set_handler 调用，很容易忘记传 ist 参数。数组的方式把所有路由集中在一起，一目了然。

**验证**：编译通过后，可以检查 IDT 条目的原始字节来确认 #DF (vector 8) 的 IST 字段确实被设为 1。在真实硬件或 QEMU 上运行不会触发 Double Fault（除非你刻意制造），所以主要是编译验证。你也可以通过在 set_handler 中打印 ist 参数来确认传递是否正确。

**关于 BP（Breakpoint，vector 3）的权限**：注意路由表中 BP 的权限是 IDTPrivilege::User 而非 Kernel。这是因为 int $3 断点指令在 Ring 3 也是合法的——调试器需要在用户态设置断点。如果你不小心把 BP 设为 Kernel-only，Ring 3 程序执行 int $3 会触发 #GP 而不是 #BP，导致调试器失效。

### Step 3: 修改 #GP 处理函数，区分用户态和内核态

**目标**：在 `handle_gp()` 中检查 `frame->cs` 的低两位，如果非零则说明中断来自 Ring 3。

**设计思路**：当 Ring 3 的用户程序执行特权指令（如 `cli`）时，CPU 触发 #GP。此时 InterruptFrame 中的 CS 字段保存的是触发异常时的 CS 值——Ring 3 代码段的低两位 = 3。通过检查这两位，我们可以区分中断来源，打印不同的消息。对于来自用户态的 #GP，打印友好的消息说明"特权隔离生效了"，然后 halt。对于来自内核态的 #GP，保持原有的 fatal halt 行为——内核态的 #GP 通常是严重的编程错误（比如访问了空指针或者调用了非法指令），不应该尝试恢复。

**实现约束**：检查方法是 `(frame->cs & 0x03) != 0`。你不需要检查 SS——x86-64 的中断帧中 CS 的 CPL 就是触发中断时的特权级。SS 在 64 位长模式下被 CPU 忽略（所有数据段基址都是 0），所以 CS 的低两位是判断 Ring 0/Ring 3 的唯一可靠方式。你需要在 handle_gp 函数中，保留现有的 dump_registers 调用，然后添加 from_user 判断，根据结果打印不同的消息。两种情况最后都调用 fatal_halt()。

**RPL vs DPL vs CPL 的区别**：这三个概念在 x86 特权级中容易混淆。CPL（Current Privilege Level）是当前运行代码的特权级，存储在 CS 的低两位。RPL（Requested Privilege Level）是选择子中请求的特权级，也是低两位——你可以把 RPL 理解为"我声称自己是哪个特权级"。DPL（Descriptor Privilege Level）是段描述符中存储的特权级，表示"这个段要求调用者至少是什么特权级"。在我们的 #GP 检测中，`frame->cs & 0x03` 取的是 CS 的 RPL 字段，而在 x86-64 中 RPL = CPL，所以可以直接用来判断当前特权级。

**验证**：这个修改在当前 tag 暂时无法直接测试（因为我们还没进入 Ring 3），但在后续章节完成 SYSRET 跳转后会自然验证。预期行为：用户程序执行 `cli` 后，串口输出类似 `[EXCEPTION] #GP at RIP=0x400000 from user mode (Ring 3)`，紧接着输出 `[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!`。如果 CS 的低两位为 0，说明异常来自内核态，输出的是 `[FATAL] General Protection Fault in kernel mode`。

**为什么不尝试恢复 Ring 3 #GP**：你可能会想，如果 #GP 来自 Ring 3，为什么不让处理函数恢复执行？答案有两层。第一，恢复到哪里？#GP 是"你执行了一条不该执行的指令"，恢复后重新执行同一条指令只会再次触发 #GP——无限循环。第二，真正的操作系统会终止触发 #GP 的进程（发送 SIGSEGV 或 SIGILL），而不是终止整个系统。但 Cinux 目前没有进程管理机制，所以只能 halt。这在后续 tag 实现进程退出后可以改进。

## 构建与运行

构建命令和之前一样：

```bash
cd build && cmake .. && make big_kernel -j$(nproc)
```

运行：

```bash
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux.img,format=raw,index=0,media=disk
```

本阶段不会有明显的输出变化——TSS IST 和 IDT 修改都是静默的基础设施改造。确认串口输出中没有新的 #GP 或 Triple Fault 即可。日志中应该能看到 `[BIG] GDT loaded (TSS with IST1 Double Fault stack)` 和 `[BIG] IDT loaded (#DF uses IST1)`。如果你看到了这些日志且没有异常输出，说明基础设施改造成功。如果出现了新的 #GP 或 Triple Fault，回顾上面的踩坑预警——最可能的原因是 TSS 结构大小不对或 IST 栈地址为 0。

## 关键常量速查

| 常量 | 值 | 含义 |
|------|----|------|
| TSS 大小 | 104 字节 | Long Mode TSS 结构（含 reserved 字段） |
| TSS type | 0x89 | Present + TSS64 Available |
| TSS GDT slots | 2 | 64 位 TSS 描述符占 16 字节（两个 GDT entry） |
| TSS GDT 选择子 | 0x38 | slot 7（GDT_TSS） |
| IST1 offset | 0x24 | TSS 结构中 ist[0] 的偏移 |
| RSP0 offset | 0x04 | TSS 结构中 rsp[0] 的偏移 |
| IOPB offset | 0x64 | TSS 结构中 iomap_base 的偏移 |
| DF 栈大小 | 4096 字节 | 1 页，编译时静态分配 |
| DF 栈对齐 | 16 字节 | x86-64 ABI 要求栈指针对齐 |

## 常见错误排查清单

完成本章后，如果遇到以下问题，按此清单排查：

| 症状 | 可能原因 | 排查方法 |
|------|----------|----------|
| 编译报 static_assert | TSS 未 packed 或字段类型错误 | 检查每个字段的类型和大小 |
| IST1 = 0 | 赋值在 ltr 之后 | 移到 TSS 描述符写入 GDT 之前 |
| #DF 触发时 Triple Fault | IST1 指向栈底而非栈顶 | 用 `sizeof(df_stack_)` 而非 `[0]` |
| 新增异常路由不生效 | 忘记给新路由加 IST 字段 | 检查 Route 数组每行是否有 5 个字段 |
| #GP handler 不区分 Ring | cs & 0x03 逻辑写反 | 确认 != 0 时 from_user = true |
| set_handler 未传 ist | 第 5 参数漏传 | 确认 set_handler 调用包含 r.ist |

## 参考资料

- Intel SDM: Vol.3A Section 5.1 — Privilege Levels
- Intel SDM: Vol.3A Section 8.2.1 — Task-State Segment (TSS) in 64-bit mode
- Intel SDM: Vol.3A Section 8.2.3 — TSS Descriptor in 64-bit mode
- Intel SDM: Vol.3A Section 6.14.5 — Interrupt Stack Table (IST)
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- Linux: arch/x86/entry/entry_64.S — IST assignment for #DF/#NMI/#MC

## 调试技巧

1. **TSS 结构大小不对**：如果编译时报 static_assert 错误，检查 TSS 结构是否用了 packed 属性。64 位 TSS 必须恰好 104 字节——这包括 reserved0 (4字节)、rsp[3] (24字节)、reserved1 (8字节)、ist[7] (56字节)、reserved2 (8字节)、reserved3 (2字节)、iomap_base (2字节)，加起来正好 104。如果多了或少了几字节，通常是某个字段的类型声明有误。
2. **IST 栈地址为 0**：如果 GDT init 后 IST1 为 0，检查栈缓冲区的声明是否正确以及 IST1 赋值代码的位置是否在 ltr 之前。IST1 应该是栈缓冲区的末尾地址（`&df_stack_[sizeof(df_stack_)]`），不是起始地址。如果写成了 `&df_stack_[0]`，IST1 指向栈底而不是栈顶，第一次 push 就可能越界。
3. **Triple Fault**：如果在后续阶段进入 Ring 3 时出现 Triple Fault，先检查 TSS.RSP0 是否被正确设置——这是最常见的 Ring 3 切换失败原因。RSP0 应该指向一个有效的、有足够空间的内核栈顶。你可以通过在 launch_first_user 中打印 RSP0 的值来验证。
4. **GDB 调试 IST**：如果需要调试 Double Fault 的 IST 栈切换，可以在 QEMU 中使用 `-d int` 参数来打印每次中断的详细信息，包括 IST 栈切换。不过这个输出非常冗长，只在需要时启用。
5. **验证 TSS 加载**：使用 `str` 指令读取当前 Task Register 值，确认它等于 GDT_TSS（0x38）。如果你看到 TR=0，说明 ltr 没有执行或 GDT 没有正确加载。可以在内核测试中通过 inline asm 验证。
6. **修改顺序很重要**：建议按 Step 1 → Step 2 → Step 3 的顺序实现。先给 GDT 加 IST 栈，再改 IDT 路由，最后改 #GP handler。每一步都单独编译验证——尤其是 TSS 结构大小的 static_assert，越早发现越好。

## 验证命令

构建并运行：

```bash
cd build && cmake .. && make big_kernel -j$(nproc)
qemu-system-x86_64 -m 8G -serial stdio -no-reboot \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux.img,format=raw,index=0,media=disk
```

检查点：
- 串口输出 `[BIG] GDT loaded (TSS with IST1 Double Fault stack).`
- 串口输出 `[BIG] IDT loaded (#DF uses IST1).`
- 无新增 #GP、#PF 或 Triple Fault

## 与后续章节的关系

本章搭建的 TSS/IST 基础设施在后续章节中会被持续使用：
- **022-2**：SYSRET 跳转后，Ring 3 代码触发异常时 CPU 自动从 TSS.RSP0 加载内核栈
- **022-3**：kernel_init_thread 中调用 `GDT::tss_set_rsp0()` 设置 Ring 3 异常时的内核栈
- **023_syscall**：SYSCALL/SYSRET 系统调用时 TSS.RSP0 用于特权级切换
- **019+**：多进程调度时每个进程需要独立的 RSP0

**修改量估算**：本章涉及约 4 个文件的修改，新增代码约 30 行（GDT 5 行 + IDT 20 行 + exception handler 5 行），没有新增文件。改动量不大，但每一行都关乎 Ring 3 切换的可靠性。建议按 Step 1 → Step 2 → Step 3 的顺序实现，每完成一步都编译验证，避免积累错误。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| TSS (Long Mode) | 104 字节，存储 RSP0-2 和 IST1-7，不保存通用寄存器 |
| IST1 | Double Fault 专用独立栈，4KB，编译时静态分配 |
| IDT IST 字段 | 中断门描述符第 3 字节低 3 位指定 IST 编号，0=不使用 |
| #GP 用户态检测 | `(frame->cs & 0x03) != 0` 区分 Ring 0/Ring 3 |
| TSS 描述符 | type=0x89 (Present + TSS64 Available)，GDT 中占 2 个 slot |
| packed 属性 | TSS 必须用 `[[gnu::packed]]`，否则编译器填充导致大小异常 |
