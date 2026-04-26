# x86_64 架构支持

> 里程碑: `009_big_kernel_entry` `010_big_kernel_gdt_idt` `011_big_kernel_pic_irq` `022_ring3_usermode`

## 功能概述

大内核的 x86_64 架构层，包括高半核启动、GDT/IDT/TSS 初始化、中断/异常处理框架、PIC/PIT 硬件编程、用户态切换、上下文切换和系统调用入口路径。

## 内核启动 (`kernel/arch/x86_64/boot.S`, `kernel/arch/x86_64/crt_stub.cpp`)
- `_start`: 切换 `%rsp` 到 `__kernel_stack_top`，清 BSS，`_init_global_ctors`，`call kernel_main`
- 链接脚本: `KERNEL_VMA = 0xFFFFFFFF80000000`，加载地址 `0x1000000`
- C++ 运行时: `__cxa_pure_virtual`、`__stack_chk_fail` (均 `cli;hlt`)，`__cxa_atexit` 返回 0

## GDT (`kernel/arch/x86_64/gdt.hpp/cpp`)
- 全局 GDT: null / kernel_code64 / kernel_data64 / user_code64 / user_data64 / TSS (16 字节双槽)
- `constexpr` 工厂: `make_null/make_code64/make_data64/make_tss`
- 选择子常量: `GDT_KERNEL_CODE=0x08`, `GDT_KERNEL_DATA=0x10`, `GDT_USER_CODE=0x1B`, `GDT_USER_DATA=0x23`, `GDT_TSS=0x28`
- `gdt_init()`: 填充 + `lgdt` + `ltr $GDT_TSS`

## IDT & 中断 (`kernel/arch/x86_64/idt.hpp/cpp`, `interrupts.S`)
- 256 个 ISR stub: `isr_noerr` (推 0 + vec) / `isr_err` (推 vec)
- `InterruptFrame`: `r15..rax + vector + error_code + rip/cs/rflags/rsp/ss`
- `using IRQHandler = void(*)(InterruptFrame*)`
- `idt_set_handler(vector, handler)` / `idt_init()`
- `isr_common`: 保存 r15..rax，`call isr_dispatch`，恢复，`iretq`
- 有 error code 的向量: 8/10/11/12/13/14/17/21

## 异常处理 (`kernel/arch/x86_64/exception_handlers.cpp`)
- `dump_registers(InterruptFrame*)`: 格式化输出所有寄存器
- `handle_pf`: 读 `%cr2`，解析页错误码
- `handle_gp`: General Protection Fault
- `handle_df [[noreturn]]`: Double Fault

## TSS (`kernel/arch/x86_64/tss.hpp`)
- `TSS [[gnu::packed]]`: RSP[3], IST[7], IOPB offset
- IST1 指向独立 4KB Double Fault 栈
- `tss_set_rsp0(kernel_stack_top)`: 每次 task 切换调用

## PIC (`kernel/arch/x86_64/pic.hpp/cpp`)
- `PIC::init(master_offset=0x20, slave_offset=0x28)`: ICW1-ICW4 + `io_wait()`
- 重映射: IRQ0-7 → 0x20-0x27, IRQ8-15 → 0x28-0x2F
- `PIC::send_eoi(irq)` / `PIC::mask(irq)` / `PIC::unmask(irq)` / `PIC::disable_all()`

## 用户态切换 (`kernel/arch/x86_64/usermode.S`)
- `jump_to_usermode(entry, user_stack, arg)`: 配置 STAR MSR，`%rcx=entry`，`%rsp=user_stack`，`%rdi=arg`，`swapgs`，`sysretq`
- 用户态 ELF 链接到 `0x400000`
- 用户栈在 `0x7FFFFF000` 附近

## 上下文切换 (`kernel/arch/x86_64/context_switch.S`)
- `context_switch(CpuContext* from, CpuContext* to)`
- 保存 callee-saved: r15/r14/r13/r12/rbp/rbx/rsp + rip (leaq .restore)
- 恢复 to 的对应字段，`jmp *56(%rsi)`

## I/O 原语 (`kernel/arch/x86_64/io.hpp`)
- 内联汇编: `io_inb/io_outb/io_inw/io_outw/io_inl/io_outl/io_wait`，`"memory"` clobber

## 内存布局 (`kernel/arch/x86_64/memory_layout.hpp`)
- 统一内核虚拟内存布局: KMEM_HEAP / MMIO / STACK / DMA / EXT2_DMA
- 所有区域通过 base + size 计算

## 分页 (`kernel/arch/x86_64/paging.hpp`)
- `union PageEntry {uint64_t raw; struct{present,writable,user,...,addr:40,nx}}`
- `FLAG_PRESENT/WRITABLE/USER/NX` 常量

## 源码位置
- `kernel/arch/x86_64/boot.S` — 内核入口
- `kernel/arch/x86_64/gdt.hpp/cpp` — GDT
- `kernel/arch/x86_64/idt.hpp/cpp` — IDT
- `kernel/arch/x86_64/interrupts.S` — ISR stubs
- `kernel/arch/x86_64/exception_handlers.cpp` — 异常处理
- `kernel/arch/x86_64/pic.hpp/cpp` — PIC
- `kernel/arch/x86_64/tss.hpp` — TSS
- `kernel/arch/x86_64/usermode.S` — 用户态切换
- `kernel/arch/x86_64/context_switch.S` — 上下文切换
- `kernel/arch/x86_64/io.hpp` — I/O 原语
- `kernel/arch/x86_64/paging.hpp` — 页表项
- `kernel/arch/x86_64/memory_layout.hpp` — 内存布局
- `kernel/arch/x86_64/crt_stub.cpp` — C++ 运行时
