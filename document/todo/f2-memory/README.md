# F2: 内存管理增强

> 为 Cinux 引入完整的虚拟内存管理：VMA 追踪、mmap、Page Cache、brk。
> 解锁后续的共享内存(F8)、动态链接器(F10)、文件系统性能(F6)。

## 实现决策

| 决策 | 选择 |
|------|------|
| VMA 数据结构 | 抽象接口（IVMAStore）+ 链表后端，可快速换红黑树 |
| Page Cache | 基于 inode + offset 的哈希表 |
| brk/sbrk | F2 做 |
| Swap | 不做 |
| Buffer Cache | 通过 Page Cache 统一（不做独立的 Buffer Cache） |

## Milestone 依赖

```
M1 VMA 抽象接口 ──→ M2 mmap/munmap/mprotect
       │                     ↓
       │               M3 brk syscall
       ↓
M4 Page Cache ──→ M5 Demand Paging 增强
       ↓
M6 ext2 Page Cache 集成

M7 伙伴系统 + Slab（独立，可与 M4 并行）
```

M1 和 M4 可并行启动。M2/M3 依赖 M1。M5/M6 依赖 M1 + M4。

## 文件清单

| 文件 | Milestone | 说明 |
|------|-----------|------|
| [00-vma.md](00-vma.md) | M1 | VMA 抽象接口 + 链表后端 |
| [01-mmap.md](01-mmap.md) | M2 | mmap/munmap/mprotect syscall |
| [02-brk.md](02-brk.md) | M3 | brk syscall + 用户态堆 |
| [03-page-cache.md](03-page-cache.md) | M4 | Page Cache 哈希表 |
| [04-demand-paging.md](04-demand-paging.md) | M5 | Demand Paging 增强 |
| [05-ext2-cache.md](05-ext2-cache.md) | M6 | ext2 Page Cache 集成 |
| [06-buddy-slab.md](06-buddy-slab.md) | M7 | 伙伴系统 + Slab 分配器 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| PMM | `kernel/mm/pmm.hpp` — bitmap 物理页分配 |
| VMM | `kernel/mm/vmm.hpp`, `kernel/mm/vmm.cpp` — 4级页表管理 |
| AddressSpace | `kernel/mm/address_space.hpp`, `.cpp` — 每进程页表 |
| Page Fault | `kernel/arch/x86_64/exception_handlers.cpp:167-276` — PF handler |
| CoW | `kernel/proc/process.cpp:489-538` — handle_cow_fault() |
| Paging flags | `kernel/arch/x86_64/paging_config.hpp` — FLAG_COW, FLAG_NX 等 |
| Memory layout | `kernel/arch/x86_64/memory_layout.hpp` — 虚拟地址区域 |
| Heap | `kernel/mm/heap.hpp`, `.cpp` — 内核堆分配器 |
| User layout | `kernel/arch/x86_64/usermode.hpp` — USER_ENTRY_BASE, USER_STACK_TOP |
| Syscall nums | `kernel/syscall/syscall_nums.hpp` — 当前 21 个 syscall |

## 验收标准

每个 Milestone：
1. `cmake --build build/` 编译通过
2. QEMU 启动 + 用户程序运行正常
3. 新增单元测试通过
4. 每文件 ≤ 500 行
