# 010-3 Read-through: 异常处理函数、集成与测试——寄存器 dump 与 int $3 验证

## 概览

本文讲解 tag 010 中大内核 C++ 异常处理函数、`kernel_main` 集成入口和 QEMU 自动化测试的完整实现。这是整个 tag 的"最后一公里"——把 010-1 的 GDT 和 010-2 的 IDT/ISR 串成一个可验证的完整链路。完成本节后，整个 tag 010 的目标就达成了：触发 `int $3` -> 串口打印完整的寄存器 dump -> 程序继续执行，不死机。

本节涉及三个源文件：`kernel/arch/x86_64/exception_handlers.cpp`（179 行，C++ 异常处理函数）、`kernel/main.cpp`（集成入口）、`kernel/test/test_gdt_idt.cpp`（自动化测试）。

关键设计决策一览：
- 致命异常 halt / 非致命异常 continue 的二分策略
- #PF 特殊处理：CR2 读缺页地址 + error code 位域解码
- 初始化顺序：kprintf_init -> GDT -> IDT -> int $3 验证
- 测试覆盖：段寄存器值、单次/多次 #BP、gate type policy

---

## 代码精讲

### exception_handlers.cpp: dump_registers 和 fatal_halt

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
    kprintf("  ... (all registers)\n");
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}

[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`dump_registers` 用 `%p` 格式输出寄存器值——虽然 `%p` 的标准语义是打印指针地址，但在内核调试场景下，把寄存器值当指针打印是最直观的方式，因为内核地址和指针地址本来就是同一套表示。`reinterpret_cast<void*>` 是为了满足 `%p` 的类型要求（`void*`），虽然这在技术上是一个 implementation-defined 的转换，但在 x86_64 的 freestanding 环境下，`uint64_t` 和 `void*` 的二进制表示是一致的。

`fatal_halt` 标记为 `[[noreturn]]`——告诉编译器这个函数永远不会返回，编译器可以据此优化调用点的代码（比如不需要在 `fatal_halt()` 调用之后放任何指令）。`cli; hlt` 放在循环里是防御性编程——虽然 `cli` 关了可屏蔽中断，但 NMI 仍然能唤醒 `hlt` 的 CPU。

### exception_handlers.cpp: #BP handler（非致命）

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}
```

#BP 是非致命异常的典范——打印完信息后直接返回，ISR stub 中的 `iretq` 会恢复 CPU 到触发断点的下一条指令。这也是为什么 #BP 使用 Trap Gate——如果用 Interrupt Gate，IF 被清除意味着中断被关闭了，如果后续代码依赖中断（比如定时器中断驱动调度），系统就会卡死。当然，我们当前还没有开中断（sti），所以这个区别暂时没有实际影响，但设计上是正确的。

### exception_handlers.cpp: #PF handler（致命 + CR2 + error code 解码）

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

#PF 的 error code 格式和其他异常不同——它有自己的专用格式（Intel SDM Vol.3A §6.13 Figure 6-8）。bit 0 表示是"页不存在"还是"权限违反"，bit 1 表示是读还是写，bit 2 表示是内核态还是用户态，bit 3 表示是否设置了保留位，bit 4 表示是否是指令取指。CR2 寄存器保存了触发缺页的虚拟地址——这是 CPU 在触发 #PF 时自动设置的，不需要在中断处理中做任何额外操作。

这些信息在调试内存管理相关的 bug 时非常有价值。比如你看到一个 "page not present read kernel" 的错误，就知道是内核态代码读了一个未映射的地址；如果看到 "protection violation write user"，就知道是用户态代码试图写一个只读的页面。

### main.cpp: kernel_main 集成

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

初始化顺序是 `kprintf_init` -> `g_gdt.init` -> `g_idt.init`——串口先初始化（后续的 kprintf 需要它），GDT 在 IDT 之前（因为 IDT 的 selector 依赖 GDT 中的 code segment），IDT 在 `int $3` 之前（否则中断触发时找不到处理程序）。注意我们**没有**调用 `sti`——因为还没有配置 IRQ 处理程序，PIT 定时器会在 `sti` 后的几毫秒内触发 IRQ 0，跳到 IDT[32]（一个空条目），导致 #GP -> Double Fault -> Triple Fault。

### test/test_gdt_idt.cpp: 集成测试

```cpp
namespace test_gdt_segments {
void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, GDT_KERNEL_CODE);
}
// ... DS, SS, ES 同理
}

namespace test_bp_exception {
void test_bp_continues_execution() {
    volatile int marker_before = 0x1234;
    __asm__ volatile("int $3");
    volatile int marker_after = 0x5678;
    TEST_ASSERT_EQ(marker_before, 0x1234);
    TEST_ASSERT_EQ(marker_after, 0x5678);
}
}

namespace test_multiple_exceptions {
void test_multiple_bp() {
    volatile uint64_t canary = 0xCAFEBABEDEADC0DEULL;
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
}
}

namespace test_idt_policy {
void test_bp_gate_is_user_trap() {
    uint8_t attr = cinux::arch::make_idt_attr(
        cinux::arch::IDTPrivilege::User,
        cinux::arch::IDTGateType::Trap);
    TEST_ASSERT_EQ(attr, 0xEFu);
}
// ... Kernel/Interrupt = 0x8E
}
```

四个测试用例覆盖了 GDT/IDT 的核心功能：段寄存器值验证（CS=0x08, DS/SS/ES=0x10）、单次 #BP 触发和恢复、多次 #BP 的栈帧完整性（canary 值不变说明 ISR 没有破坏栈）、以及 IDT gate type policy 的编译期验证。

---

## 设计决策

### 决策：致命/非致命二分策略 vs 统一 halt

**问题**: 异常处理应该 halt 还是尝试恢复？

**本项目的做法**: 只把 #BP 和 #DB 视为非致命（打印后返回继续执行），其余所有异常一律 fatal_halt。#PF 虽然理论上可以通过按需分页恢复，但在当前没有 PMM/VMM 的阶段，直接 halt 是最安全的做法。

**备选方案**: 所有异常统一 fatal_halt，不做非致命/致命的区分。

**为什么不选备选方案**: `int $3` 断点是我们验证中断链路的关键手段——如果 #BP 也 halt，内核执行到 `int $3` 就停住了，无法验证 IRETQ 后程序能否正常继续。而且断点异常在调试场景下本来就应该允许程序继续执行。

---

## 扩展方向

1. **(2 星)** 在 #PF handler 中添加符号解析——维护一个函数地址范围表，#PF 时根据 RIP 查找对应的函数名并打印。

2. **(3 星)** 实现异常恢复而非永久停机——#PF 时尝试分配物理页并映射，#GP 时向进程投递 SIGSEGV 信号。

3. **(3 星)** 实现中断嵌套——在 Trap Gate handler 中短暂开中断，允许高优先级中断打断当前处理。

---

## 参考资料

- Intel SDM: Vol.3A §6.13 — Error Codes (pp.6-18~6-19)
- Intel SDM: Vol.3A §6.14.2 — 64-Bit Mode Stack Frame (p.6-21)
- OSDev Wiki: [Exceptions](https://wiki.osdev.org/Exceptions) — 所有 CPU 异常的向量号、error code 和触发条件汇总
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — trap dispatch, handlers
- Linux: [arch/x86/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/traps.c) — do_page_fault, do_general_protection
- SerenityOS: [Kernel/Arch/x86_64/Interrupts/APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) — OOP interrupt framework
