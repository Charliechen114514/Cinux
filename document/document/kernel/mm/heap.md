# 堆管理 (Heap)

> 里程碑: `017_mm_heap`

## 功能概述

内核堆分配器，first-fit 策略 + block 分裂/合并。free list 耗尽时通过 VMM 自动扩展。接管全局 `new`/`delete` 操作符。

## 数据结构
```cpp
struct BlockHeader [[gnu::packed]] {
    uint32_t magic;    // 0xDEADBEEF
    uint32_t size;
    bool free;
    uint8_t _pad[7];
    BlockHeader* next;
};
```

## API (`kernel/mm/heap.hpp/cpp`)
- `Heap::init(virt_base, initial_size)` — 初始化堆区域
- `Heap::alloc(size, align=16) → void*` — 分配 (first-fit + split)
- `Heap::free(ptr)` — 释放 (magic 校验 + coalesce)
- `Heap::dump_stats()` — 调试信息

### 全局操作符
- `operator new/new[]/delete/delete[]`
- `operator new(size, align_val_t)` — 对齐分配

## 自动扩展
- free list 耗尽时调 `VMM::map()` 扩展堆

## 线程安全
- Spinlock 保护: `alloc/free` 持锁 (TIER 0)
- double-free 触发 magic 校验 panic

## 源码位置
- `kernel/mm/heap.hpp/cpp`
