---
title: 010-big-kernel-gdt-idt-3 · GDT/IDT 重构
---

# 010-3 教程：异常处理函数与集成验证——让内核学会"遗言"

> 标签：x86_64, 异常处理, 寄存器 dump, CR2, int $3, IRETQ, 致命异常, QEMU 测试
> 前置：[010-2 大内核 IDT + ISR 汇编跳板](./010-big-kernel-gdt-idt-2.md)

## 前言

前两节我们搭好了 GDT、IDT 和 ISR 汇编跳板——IDT 的 14 个路由条目全部注册完毕，ISR stub 也实例化好了。但 ISR stub 里的 `call \handler` 调用的 C 函数还没实现。本节是整个 tag 010 的"最后一公里"——实现 15 个 C++ 异常处理函数（打印寄存器 dump + 判断致命/非致命），然后把所有东西集成到 `kernel_main` 里用 `int $3` 触发一个断点异常来验证整个链路。

完成本节后的效果是这样的：我们在 `kernel_main` 里执行一条 `int $3` 指令触发断点异常，串口会输出完整的寄存器 dump（RAX-R15、RIP、CS、RFLAGS、RSP、SS、Error Code），然后打印 "Breakpoint returned, continuing."——程序继续执行，不死机。

## 环境说明

和前两节完全一致——WSL2 + GCC 13+ + CMake + QEMU，x86_64 freestanding 内核。本节的代码主要在 `exception_handlers.cpp`（C++ 异常处理函数）和 `kernel/main.cpp`（集成入口）中。

## 第一步——异常处理函数：致命的 halt，不致命的 continue

异常处理函数在 `kernel/arch/x86_64/exception_handlers.cpp` 中实现。所有函数都声明为 `extern "C"` linkage——这是因为 ISR stub 中的 `call \handler` 使用的是 C 风格的函数调用（不经过 C++ name mangling），所以 handler 的符号名必须和汇编中的一致。

核心辅助函数 `dump_registers` 负责格式化输出所有寄存器的值：

```cpp
void dump_registers(const InterruptFrame* frame,
                    const char* name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", name, vector);
    kprintf("  RIP   = %p   CS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rip),
            static_cast<unsigned>(frame->cs));
    kprintf("  RFLAGS= %p\n",
            reinterpret_cast<void*>(frame->rflags));
    kprintf("  RSP   = %p   SS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rsp),
            static_cast<unsigned>(frame->ss));
    kprintf("  RAX=%p  RBX=%p\n", ...);
    kprintf("  ... (all GP registers)\n");
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}
```

用 `%p` 格式输出寄存器值——虽然 `%p` 的标准语义是打印指针地址，但在内核调试场景下，把寄存器值当指针打印是最直观的方式。`reinterpret_cast<void*>` 是为了满足 `%p` 的类型要求。

然后是 15 个 handler 函数。它们分为两类：非致命的（#BP 和 #DB）打印完后直接返回，ISR stub 中的 iretq 会恢复执行；致命的打印完后调用 `fatal_halt()` 进入 `cli; hlt` 死循环。

非致命的 #BP handler 是我们验证整个链路的关键：

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}
```

打印完信息后直接返回——ISR stub 中的 pop + addq + iretq 会把 CPU 恢复到触发 `int $3` 的下一条指令。这就是 "breakpoint returned, continuing" 的意思。

#PF handler 是最复杂的——它不仅打印寄存器 dump，还从 CR2 读出触发缺页的虚拟地址，然后解码 error code 的各个 bit：

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_registers(frame, "#PF", 14);
    kprintf("[FATAL] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[FATAL] Faulting address (CR2) = %p -- halting.\n",
            reinterpret_cast<void*>(fault_addr));
    fatal_halt();
}
```

#PF 的 error code 格式（Intel SDM Vol.3A §6.13）是专用的——bit 0 表示"页不存在"还是"权限违反"，bit 1 表示读还是写，bit 2 表示内核态还是用户态。CR2 寄存器保存了触发缺页的虚拟地址，这是 CPU 在触发 #PF 时自动设置的。这些信息在后续调试内存管理相关的 bug 时非常有价值。

## 第二步——IDT 初始化：数据驱动的路由表

`kernel/arch/x86_64/idt.cpp` 负责把 ISR stub 和 C handler 连接到 IDT 条目中。核心设计是"数据驱动的路由表"——一个 `Route` 结构体数组，每个元素包含 {向量号, stub 地址, 特权级, 门类型}，遍历数组配置 IDT：

```cpp
void IDT::init() {
    for (auto& entry : entries_) {
        entry = Entry{};
    }

    struct Route {
        ExceptionVector vector;
        Stub stub;
        IDTPrivilege priv;
        IDTGateType gate;
    };

    const Route routes[] = {
        {ExceptionVector::DE,  isr_de_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::DB,  isr_db_stub,  IDTPrivilege::Kernel, IDTGateType::Trap},
        {ExceptionVector::NMI, isr_nmi_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::BP,  isr_bp_stub,  IDTPrivilege::User,   IDTGateType::Trap},
        /* ... vectors 4-14 ... */
    };

    for (const auto& r : routes) {
        set_handler(r.vector, r.stub, GDT_KERNEL_CODE,
                    make_idt_attr(r.priv, r.gate), 0);
    }

    idtr_.limit = static_cast<uint16_t>(sizeof(entries_) - 1);
    idtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

这个路由表一目了然——14 条路由，每条包含了向量号、ISR stub 地址、特权级和门类型的完整信息。对比 xv6 的 `tvinit()` 用循环 + `SETGATE` 宏逐个配置 256 个条目的方式，我们的路由表更紧凑。

`set_handler()` 函数把 64 位处理程序地址拆成三段存入 IDT 条目（offset_low 16 位 + offset_mid 16 位 + offset_high 32 位），selector 固定为 `GDT_KERNEL_CODE`（0x08），IST 为 0。

`load()` 就一行——`lidt %[idtr]`，从内存加载 IDTR。比 GDT 的 `load()` 简单得多，因为不需要刷新段寄存器。

## 第三步——集成到 kernel_main：int $3 触发验证

最后一步是把所有东西集成到 `kernel_main` 中：

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

初始化顺序是 `kprintf_init` -> `g_gdt.init` -> `g_idt.init`——串口先初始化，GDT 在 IDT 之前（因为 IDT 的 selector 依赖 GDT），IDT 在 `int $3` 之前。注意我们**没有**调用 `sti`——因为没有配置 IRQ 处理程序，PIT 定时器会在 `sti` 后的几毫秒内触发 IRQ 0，跳到 IDT[32]（空条目），导致 #GP -> Double Fault -> Triple Fault。

`int $3` 是一字节指令（opcode 0xCC），被设计成一字节是有原因的——调试器可以在任意位置插入断点（只需覆盖一个字节），而不需要考虑指令对齐问题。不过对我们来说，`int $3` 和 `int $N`（N != 3）的效果是一样的——都是触发 IDT[3] 对应的处理程序。

## 验证

运行大内核，串口输出应该是这样的：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...   CS  = 0x0008
  RFLAGS= 0x...
  RSP   = 0x...   SS  = 0x0010
  RAX=0x...  RBX=0x...
  ... (all registers)
  ERROR CODE = 0x0
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

自动化的 QEMU 测试（8 个测试全部 PASS）：

```bash
cmake --build build --target run-big-kernel-test
```

## 收尾

到这里，大内核终于不再是"聋哑人"了。CPU 遇到异常时，它会通过 IDT 找到我们的处理程序，ISR stub 保存寄存器，C handler 打印完整的寄存器 dump，非致命异常后程序还能继续执行。这是内核开发中最基础但也最关键的基础设施——没有它，你后续写的任何代码都可能在任何时刻被 CPU 异常杀死，而你连出问题的地方都不知道。

但这只是开始。目前我们只处理了 CPU 异常（向量 0-14），硬件中断（IRQ 0-15，向量 32-47）还没有处理。下一章（011）我们要配置 8259 PIC（可编程中断控制器），把硬件中断映射到 IDT 的向量 32-47，然后实现定时器中断——让串口每秒输出一个 `[TICK] uptime: Ns`。那才是内核真正"活"起来的时刻。

## 参考资料

- Intel SDM: Vol.3A §6.13 — Error Codes (pp.6-18~6-19)
- Intel SDM: Vol.3A §6.14.2 — 64-Bit Mode Stack Frame (p.6-21)
- OSDev Wiki: [Exceptions](https://wiki.osdev.org/Exceptions) — 所有 CPU 异常的向量号、error code 和触发条件汇总
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — tvinit, trap dispatch
- Linux: [arch/x86/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/traps.c) — trap setup, IST for DF/MC
- Linux: [arch/x86/entry/entry_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S) — idtentry macro
- SerenityOS: [Kernel/Arch/x86_64/Interrupts/APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) — OOP interrupt framework
