# 地址空间 (Address Space)

> 里程碑: `018_mm_address_space`

## 功能概述

每个进程独立的虚拟地址空间抽象。创建新 PML4 并复制内核高半部分 (PML4[256-511])，实现用户区隔离。

## 核心类 (`kernel/mm/address_space.hpp/cpp`)

```cpp
class AddressSpace {
    uint64_t pml4_phys_;
    static uint64_t* kernel_pml4_;

    // 禁止拷贝
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;
};
```

### 生命周期
- `static init_kernel()` — 读 CR3 保存为 `kernel_pml4_`
- 构造器 — alloc 新 PML4，复制 PML4[256-511] 内核条目
- 析构器 — 遍历用户区 PML4[0-255] 逐级释放

### API
- `map(virt, phys, flags)` — 在此地址空间映射
- `unmap(virt)` — 解除映射
- `activate()` — `mov CR3, pml4_phys_`

## 验证
- 创建 AS#1 和 AS#2
- 在 AS#1 映射一页，切换到 AS#2
- translate 该地址返回 0 → 隔离成功

## 集成
- `Task::addr_space` 指向进程的 AddressSpace
- 用户态进程通过 `execve()` 构建新地址空间

## 源码位置
- `kernel/mm/address_space.hpp/cpp`
