# 物理内存管理 (PMM)

> 里程碑: `015_mm_pmm`

## 功能概述

大内核的 Bitmap 物理内存分配器。解析 E820 内存图，管理 4KB 物理页面，支持单页和连续多页分配/释放。

## 初始化
1. `parse_memory_map(BootInfo&, regions[], max)` — 提取 type=1 可用区域，过滤低 1MB，对齐到 4KB
2. Bitmap 放于 `__kernel_end` 对齐后
3. 先全标记已用，再将可用区域清零，最后标记内核 + bitmap 自身为已用

## API (`kernel/mm/pmm.hpp/cpp`)
- `PMM::init(BootInfo&)` — 初始化
- `alloc_page() → uint64_t` — 分配一页，返回物理地址 (0=OOM)
- `free_page(uint64_t phys)` — 释放一页
- `alloc_pages(count)` — 分配连续多页
- `free_pages(phys, count)` — 释放连续多页
- `free_page_count()` / `total_page_count()` — 统计信息

## 性能优化
- `alloc_page` 用 `__builtin_ctzll` 加速 bit 扫描 (64 位一组)

## 线程安全
- Spinlock 保护: `alloc_page/alloc_pages/free_page/free_pages` 持锁 (TIER 0)

## 源码位置
- `kernel/mm/pmm.hpp/cpp`
