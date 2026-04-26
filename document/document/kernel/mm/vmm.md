# 虚拟内存管理 (VMM)

> 里程碑: `016_mm_vmm`

## 功能概述

4 级页表 (PML4→PDPT→PD→PT) 操作层，提供虚拟地址的 map/unmap/translate，缺中间级时自动分配新页表。

## 页表项 (`kernel/arch/x86_64/paging.hpp`)
```cpp
union PageEntry {
    uint64_t raw;
    struct {
        uint64_t present:1, writable:1, user:1, pwt:1, pcd:1,
                 accessed:1, dirty:1, huge:1, global:1, _avail:3,
                 addr:40, nx:1;
    };
};
```
- `FLAG_PRESENT / FLAG_WRITABLE / FLAG_USER / FLAG_NX`

## API (`kernel/mm/vmm.hpp/cpp`)
- `VMM::map(virt, phys, flags, pml4=nullptr)` — 缺中间级时 `PMM::alloc_page()` 新建并清零
- `VMM::unmap(virt, pml4=nullptr)` — 解除映射
- `VMM::translate(virt) → uint64_t` — 虚拟地址转物理地址
- `flush_tlb(virt)` — `invlpg`
- `flush_tlb_all()` — reload CR3

## 缺页处理
- `#PF handler` 更新: 读 `%cr2`，尝试按需分配，无法处理时 panic

## 线程安全
- Spinlock 保护: `map/unmap` 持锁 (TIER 2)

## 源码位置
- `kernel/mm/vmm.hpp/cpp`
- `kernel/arch/x86_64/paging.hpp` — PageEntry 定义
