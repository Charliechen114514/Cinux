---
title: Higher-Half 修复
---

# 019 · Higher-Half 内核修复

## 问题

大内核运行在恒等映射地址（0x1000000）而非 higher-half（0xFFFFFFFF81000000），原因是 mini kernel 的 ELF 加载器将入口点中的 higher-half 偏移去掉了：

```cpp
// elf_loader.cpp — 修改前（错误）
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;  // 0xFFFFFFFF81000000 → 0x1000000
}
```

这导致所有用户 AddressSpace 共享 PML4[0]（恒等映射），破坏了进程隔离：在一个地址空间中创建的页表项会通过共享的 PDPT 子树泄漏到其他地址空间中。

## 根本原因

引导加载程序设置了两个映射，指向同一个 PDPT/PD：

```
PML4[0]   → PDPT[0]   → PD → 2MB 页（恒等映射）
PML4[511] → PDPT[510] → PD → 2MB 页（higher-half）
```

`identity_map_up_to()` 填充 PD 表项，自动覆盖了两条路径。higher-half 映射已经就位，只是加载器没有使用它。

## 修复

1. **elf_loader.cpp**：直接返回 `saved_entry`（higher-half 虚拟地址）。
2. **address_space.cpp**：停止将 PML4[0] 复制到新地址空间。构造函数仅复制 PML4[256..511]（内核 higher-half 部分）。
3. **test_address_space.cpp**：移除之前的变通方案——隔离测试现在可以对任意地址工作，激活测试不再需要内联汇编技巧。
