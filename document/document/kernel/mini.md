# 小内核 (Bootstrap Kernel)

> 里程碑: `005_mini_kernel_entry` `006_mini_kernel_pmm` `007_mini_kernel_interrupts` `008_mini_kernel_disk_and_loader`

## 功能概述

小内核是一个最小化的 C++ 内核，职责是初始化硬件 → 提供基础服务（串口输出、内存分配、磁盘读写）→ 加载并跳转到**大内核**。运行在物理地址 `0x20000` (128KB)，大内核加载到 `0x1000000` (16MB)。

## 子系统

### 串口 & kprintf (`kernel/mini/driver/serial.hpp/cpp`, `kernel/mini/lib/kprintf.hpp/cpp`)
- `Serial::init(port, baud)` / `Serial::putc(c)` / `Serial::puts(s)`
- `kprintf` / `kvprintf`: 简化版，支持 `%d %u %x %X %s %p %c %%`

### 物理内存管理 (`kernel/mini/mm/pmm.hpp/cpp`)
- Bitmap 物理内存分配器
- `init(BootInfo&)`: 解析 E820，初始化 bitmap
- `alloc_page() → uint64_t` (返回物理地址，0=OOM)
- `free_page(uint64_t phys)`
- Bitmap 放在小内核末端 (`__mini_kernel_end` 对齐后)
- 过滤低 1MB，标记小内核自身和 bitmap 为已用

### 中断处理 (`kernel/mini/arch/x86_64/gdt.hpp/cpp`, `idt.hpp/cpp`, `interrupts.S`)
- 基础 GDT: null / code64 / data64
- 简化 IDT: 仅配置 #BP(3) 和 #PF(14)
- `handle_bp(InterruptFrame*)`: 打印断点异常 + 寄存器 dump
- `handle_pf(InterruptFrame*)`: 读 CR2，解析页错误码，打印详细信息

### 磁盘 & ELF 加载器 (`kernel/mini/driver/ata.hpp/cpp`, `kernel/mini/elf_loader.hpp/cpp`, `kernel/mini/big_kernel_loader.hpp/cpp`)
- ATA PIO 磁盘驱动: `init()` / `read(lba, count, buffer)`
- ELF64 解析器: `parse_elf_header` / `calculate_kernel_size` / `load_elf`
- `load_big_kernel(disk_lba) → uint64_t`: 读取大内核到 `0x1000000`，返回 entry_point

## C++ Freestanding 运行时 (`kernel/mini/arch/x86_64/crt_stub.cpp`)
- `__cxa_pure_virtual` / `__stack_chk_fail` — `cli; hlt`
- `__cxa_atexit` — 空实现
- `_init_global_ctors` — 遍历 `.init_array` 调用全局构造函数
- `operator new/delete` — freestanding stub (`halt`)
- 已验证: 虚函数/vtable 多态、全局对象构造均正常

## 启动流程 (`kernel/mini/main.cpp`)
1. 初始化串口
2. 初始化 PMM
3. 初始化 IDT (#BP/#PF)
4. 初始化 ATA
5. 加载大内核: `entry = load_big_kernel(BIG_KERNEL_LBA)`
6. 输出 `[MINI] Jumping to big kernel at 0x...`
7. 跳转: `jmp *%rax`

## 常量
```cpp
constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;
constexpr uint64_t BIG_KERNEL_LOAD_ADDR  = 0x1000000;
```

## 源码位置
- `kernel/mini/main.cpp` — 小内核入口
- `kernel/mini/linker.ld` — 链接脚本 (物理地址 0x20000)
- `kernel/mini/arch/x86_64/` — GDT、IDT、C++ 运行时
- `kernel/mini/driver/` — 串口、ATA 磁盘
- `kernel/mini/lib/` — kprintf
- `kernel/mini/mm/` — PMM
