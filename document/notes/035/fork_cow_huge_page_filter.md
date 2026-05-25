---
title: Fork CoW Huge Page
---

# 035 fork() CoW 与 Huge Page 过滤：方案 A 实施前安全审查

## 背景

Cinux 的 PML4[0] 混合了内核恒等映射（无 FLAG_USER）和用户私有映射（有 FLAG_USER）。
fork() 当前复制整个 PML4[0..255]，包括不应该复制的内核映射（1GB MMIO 大页、2MB RAM 大页）。

**方案 A**：在 `copy_page_table_level` 中按 `FLAG_USER` 过滤，跳过内核映射条目。
子进程通过共享的 PML4[256..511]（高半区）访问硬件。

实施前提：**所有内核代码必须通过高半区地址访问硬件，不依赖 PML4[0] 恒等映射。**

## 逐项审查结果

### 1. `phys_to_virt()` — 安全

三处实现（vmm.cpp、address_space.cpp、process.cpp）一致：

```cpp
return reinterpret_cast<PageEntry*>(phys + 0xFFFFFFFF80000000ULL);
```

正确使用高半区偏移。

### 2. CoW 页拷贝 — 安全

process.cpp 的 `handle_cow_fault()`：

```cpp
auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
```

源页和目标页都通过 `phys + KERNEL_VMA` 访问，不依赖恒等映射。

### 3. AHCI 驱动 — 安全

`AHCI::map_bar5()` 使用 `KMEM_MMIO_BASE`（高半区）映射 BAR5：
通过 `g_vmm.map(virt, phys, ...)` 创建独立映射，DMA 结构体也用 `phys + 0xFFFFFFFF80000000` 访问。

### 4. APIC — N/A

代码库中不存在 APIC 实现。

### 5. Framebuffer — 阻塞项（已修复）

原实现：

```cpp
// framebuffer.cpp（修复前）
addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys);  // 0xFD000000 恒等映射！
```

这是唯一的阻塞项。Framebuffer 驱动直接将物理地址 `0xFD000000` 当作虚拟地址使用，
完全依赖 PML4[0] 的恒等映射。

## Framebuffer 修复过程与踩坑

### 第一版：直接加 KERNEL_VMA 偏移 — 黑屏

```cpp
addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys + KERNEL_VMA);
// 0xFD000000 + 0xFFFFFFFF80000000 = 0xFFFFFFFFFD000000
```

**结果**：黑屏。`0xFFFFFFFFFD000000` 在高半区页表中没有对应的页表映射。
高半区直映射只覆盖了内核代码段所在的物理 RAM 范围，不覆盖 MMIO 地址（0xFD000000）。

### 第二版：用 VMM 映射 4KB 页到 KMEM_FB_BASE — 极慢

```cpp
for (uint64_t i = 0; i < num_pages; i++)
    g_vmm.map(KMEM_FB_BASE + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE, mmio_flags);
```

**结果**：画面恢复但性能暴跌。8MB framebuffer 需要 ~2048 个 4KB 页表条目，
远超 L1/L2 TLB 容量（约 64+512 条），导致 TLB 抖动，每次像素写入都触发页表遍历。

### 第三版：VMM 新增 `map_2mb()` — 仍有性能问题

给 VMM 加了 `map_2mb()` 方法，用 2MB 大页映射。TLB 条目从 ~2048 降到 ~4 个。

**结果**：略有缓解但仍抖动。原因：映射时加了 `FLAG_PCD`（Cache Disable）。
原 `map_mmio()` 使用 `FLAG_PRESENT | FLAG_WRITABLE`（无 PCD），framebuffer 是缓存写回的。
加了 PCD 后每次像素写入都直通内存，在 QEMU 中极慢。

### 最终版：去掉 FLAG_PCD — 修复

```cpp
constexpr uint64_t mmio_flags = arch::FLAG_PRESENT | arch::FLAG_WRITABLE;
```

去掉 PCD 后性能恢复正常。QEMU 的 framebuffer 就是 host 内存，缓存写回完全可行。

## 最终改动汇总

| 文件 | 改动 |
|------|------|
| `memory_layout.hpp` | 新增 `KERNEL_VMA` 共享常量 + `KMEM_FB_SIZE/BASE` 区域 |
| `vmm.hpp/cpp` | 新增 `map_2mb()` 方法 |
| `framebuffer.cpp` | 用 `g_vmm.map_2mb()` 映射到 `KMEM_FB_BASE`，不再依赖 PML4[0] |

## 结论

方案 A 的安全审查通过。唯一的阻塞项（Framebuffer 恒等映射）已修复。
所有内核硬件访问路径（CoW、AHCI、Framebuffer）现在都走高半区映射。

**可以安全实施 `FLAG_USER` 过滤。**
