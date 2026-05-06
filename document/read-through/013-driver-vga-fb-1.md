# 013-1 通读：底座——线性帧缓冲区与 MMIO 页表映射

## 概览

本文是 tag `013_driver_vga_fb` 三篇通读教程的第一篇，聚焦于帧缓冲区驱动和 MMIO 页表映射这两个底层基础设施。在整个 tag 的架构中，这一层是最底部的"地基"——没有帧缓冲区的像素写入能力，上层的字体渲染和控制台就无从谈起；而没有 `map_mmio()` 把帧缓冲区物理地址映射进虚拟地址空间，连第一个像素都写不了。

我们从两个方向展开：先是 `paging.cpp` 中的 `map_mmio()` 函数，它负责在已有页表结构中添加大页映射条目；然后是 `Framebuffer` 类，它把映射后的物理内存包装成一套简洁的绘图原语。两者加起来大约 200 行代码，但覆盖了 x86_64 页表操作和 MMIO 设备访问两个非常重要的底层概念。

## 架构图

```
kernel_main
    │
    ├── BootInfo (物理 0x7000)
    │       │
    │       └── fb_addr, fb_width, fb_height, fb_pitch, fb_bpp
    │
    ├── arch::map_mmio(fb_phys, fb_size)
    │       │
    │       ├── PD (0xFFFFFFFF80003000)  ← 填充 2MB 大页条目
    │       └── PDPT (0xFFFFFFFF80002000) ← 填充 1GB 大页条目
    │
    └── Framebuffer::init(BootInfo)
            │
            ├── 调用 map_mmio() 映射物理帧缓冲区
            ├── addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys)
            └── 后续绘图操作通过 addr_ 直接写入像素

    内存视角（32bpp, 1024x768）:

    fb_addr ──► [pixel(0,0)][pixel(1,0)]...[pixel(1023,0)]  ← 第 0 行
                [pixel(0,1)][pixel(1,1)]...[pixel(1023,1)]  ← 第 1 行
                ...
                [pixel(0,767)]...              [pixel(1023,767)] ← 第 767 行
                ^                                        ^
                fb_addr                    fb_addr + pitch * height
```

## 代码精讲

### paging.hpp —— MMIO 映射的接口声明

```cpp
#pragma once

#include <stdint.h>

namespace cinux::arch {

void map_mmio(uint64_t phys, uint64_t size);

}  // namespace cinux::arch
```

这个头文件非常精简，只暴露了一个函数。之所以把 paging 相关的功能放在 `arch` 命名空间下，是因为页表操作是架构相关的——如果在 ARM 或 RISC-V 上跑，这里的实现完全不同。函数签名接收一个物理基地址和大小，不需要返回值，因为我们使用 identity mapping，映射后的虚拟地址就等于物理地址。

### paging.cpp —— 大页映射的实现

这是本篇最核心的代码，我们逐段来看。

```cpp
namespace {

constexpr uint64_t PD_HUGE_PAGE_FLAGS = 0x83;
constexpr uint64_t PDPT_1GB_PAGE_FLAGS = 0x83;
constexpr uint64_t PAGE_2MB_SIZE = 0x200000;
constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;
constexpr uint32_t PT_ENTRIES = 512;

constexpr uint64_t PD_VIRT_ADDR = 0xFFFFFFFF80003000ULL;
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;
constexpr uint32_t PDPT_PD_ENTRY = 0;
```

匿名命名空间里的常量定义了页表操作所需的一切参数。`0x83` 这个标志值拆开来看是三个位的组合：bit 0 (Present = 1)、bit 1 (Read/Write = 1)、bit 7 (Page Size = 1)。Page Size 位置 1 是告诉 CPU 这是一个大页条目而不是指向下一级页表的指针，Intel SDM Vol.3A 表 4-18 详细描述了 2MB 大页的 PDE 格式，表 4-16 描述了 1GB 大页的 PDPTE 格式。

PD 和 PDPT 的虚拟地址是 bootloader 在进入 long mode 时预设的 higher-half 映射地址。`PDPT_PD_ENTRY = 0` 是一个安全守卫——PDPT[0] 指向我们的 PD，如果覆盖了这个条目，所有低地址映射全部失效。

```cpp
bool has_1gb_pages() {
    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;
}

void reload_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

`has_1gb_pages()` 通过 CPUID 扩展功能号 0x80000001 检查 EDX 的 bit 26（Intel SDM Vol.3A §4.1.4）。不是所有 x86_64 CPU 都支持 1GB 大页——QEMU 默认的 TCG 模式就不一定支持，所以这个检查是必要的。

`reload_cr3()` 通过重新加载 CR3 来刷新全部 TLB。这是一种"核弹级"的刷新方式——对于 2MB 大页我们用更精确的 `invlpg` 指令，但对于修改 PDPT 条目（影响整个 1GB 地址空间），必须重新加载 CR3。

```cpp
void map_mmio(uint64_t phys, uint64_t size) {
    auto* pd = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    uint64_t end = phys + size;
```

函数开头把 PD 和 PDPT 的虚拟地址转成 `volatile uint64_t*`。这里的 volatile 很关键——页表条目是硬件也会访问的数据结构，编译器绝不能优化掉对它们的写入。

```cpp
    uint64_t cur = phys & ~(PAGE_2MB_SIZE - 1);
    while (cur < end && cur < PAGE_1GB_SIZE) {
        uint32_t idx = static_cast<uint32_t>(cur / PAGE_2MB_SIZE);
        if (idx < PT_ENTRIES && pd[idx] == 0) {
            pd[idx] = cur | PD_HUGE_PAGE_FLAGS;
            __asm__ volatile("invlpg (%0)" : : "r"(cur));
        }
        cur += PAGE_2MB_SIZE;
    }
```

第一部分处理物理地址在 1GB 以内的范围，用 2MB 大页映射。`phys & ~(PAGE_2MB_SIZE - 1)` 把物理地址向下对齐到 2MB 边界——因为大页必须对齐到自身大小。循环中先检查目标条目是否为空（`pd[idx] == 0`），避免覆盖已有映射。写入条目后立即用 `invlpg` 刷新该虚拟地址对应的 TLB 条目，确保后续访问使用新的映射。

一个 1024x768x32bpp 的帧缓冲区大约 3MB，需要 2 个 2MB 大页来覆盖。Bochs 默认帧缓冲区物理地址是 0xE0000000（3.5GB），超出了 1GB 范围，所以需要走到第二部分。

```cpp
    if (end > PAGE_1GB_SIZE && has_1gb_pages()) {
        uint64_t cur1g = phys & ~(PAGE_1GB_SIZE - 1);
        if (cur1g < PAGE_1GB_SIZE) cur1g = PAGE_1GB_SIZE;

        while (cur1g < end) {
            uint32_t n = static_cast<uint32_t>(cur1g / PAGE_1GB_SIZE);
            if (n < PT_ENTRIES && n != PDPT_PD_ENTRY && pdpt[n] == 0) {
                pdpt[n] = cur1g | PDPT_1GB_PAGE_FLAGS;
            }
            cur1g += PAGE_1GB_SIZE;
        }
        reload_cr3();
    }
```

第二部分处理超过 1GB 的范围，用 1GB 大页映射到 PDPT 中。`cur1g` 同样需要向下对齐，但特别地，如果对齐后的地址落在 1GB 以内，就强制跳到 1GB 边界（因为第一部分已经处理了 1GB 以内的区域）。这里有一个三重安全检查：`n != PDPT_PD_ENTRY` 确保不覆盖 PDPT[0]（指向 PD 的条目），`pdpt[n] == 0` 避免覆盖已有映射。修改 PDPT 后调用 `reload_cr3()` 刷新全局 TLB。

### framebuffer.hpp —— Framebuffer 类声明

```cpp
namespace cinux::drivers {

class Framebuffer {
public:
    void init(const BootInfo& bi);
    void put_pixel(uint32_t x, uint32_t y, uint32_t argb);
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t argb);
    void scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg);
    void clear(uint32_t argb = 0);
    uint32_t get_pixel(uint32_t x, uint32_t y) const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch() const { return pitch_; }

private:
    volatile uint32_t* addr_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t pitch_ = 0;
    uint32_t bpp_ = 0;
};

}  // namespace cinux::drivers
```

Framebuffer 类的接口设计很直观——六个公开方法覆盖了基本绘图需求，三个 getter 暴露尺寸信息。私有成员 `addr_` 是 `volatile uint32_t*` 类型，这是 MMIO 编程的铁律：指向设备寄存器或帧缓冲区的指针必须是 volatile，因为编译器不知道硬件也在读写这块内存，更不能假设"写入后马上读回的值一定等于刚写入的值"。

注意 `pitch_` 的语义是"每扫描行的字节数"而不是像素数。这个值可能大于 `width_ * 4`，因为某些显卡在行尾添加对齐填充。`bpp_` 在当前实现中只做记录，实际像素操作都假设是 32bpp（4 字节/像素）。

### framebuffer.cpp —— Framebuffer 类实现

```cpp
void Framebuffer::init(const BootInfo& bi) {
    uint64_t fb_phys = bi.fb_addr;
    width_           = bi.fb_width;
    height_          = bi.fb_height;
    pitch_           = bi.fb_pitch;
    bpp_             = bi.fb_bpp;

    uint64_t fb_size = static_cast<uint64_t>(pitch_) * height_;
    arch::map_mmio(fb_phys, fb_size);

    addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys);
}
```

`init()` 是整个帧缓冲区驱动的入口。它从 BootInfo 中提取五个参数，计算帧缓冲区总大小（`pitch * height`，不是 `width * height * 4`——pitch 才是正确的行宽），然后调用 `map_mmio()` 映射这片物理内存。映射完成后，由于我们使用 identity mapping，物理地址可以直接作为虚拟地址使用——这就是最后一行 reinterpret_cast 能工作的原因。

这里有一个微妙之处：`fb_size` 用 `pitch_` 而不是 `width_ * 4` 来计算。如果 pitch 等于 `width * 4`，结果相同；但如果 pitch 更大（行尾有填充），用 width 计算就会少映射一部分，导致访问行尾填充区域时触发 page fault。

```cpp
void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= width_ || y >= height_)
        return;
    addr_[y * (pitch_ / 4) + x] = argb;
}
```

`put_pixel` 是最基础的绘图操作。像素地址的计算公式是 `y * (pitch_ / 4) + x`——先把 pitch 从字节数转换成 uint32_t 的个数（除以 4），然后乘以行号加上列号。边界检查用 `>=` 而不是 `>`，因为坐标是 0-indexed 的。返回的 void——超出边界就静默忽略，这在图形编程中是合理的，因为裁剪是高频操作。

```cpp
void Framebuffer::scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg) {
    if (lines >= height_) {
        clear(bg);
        return;
    }

    auto*                   buf       = reinterpret_cast<volatile uint8_t*>(addr_);
    const volatile uint8_t* src       = buf + static_cast<uint32_t>(pitch_) * lines;
    volatile uint8_t*       dst       = buf;
    uint32_t                move_bytes = (height_ - lines) * pitch_;
    for (uint32_t i = 0; i < move_bytes; i++) {
        dst[i] = src[i];
    }

    uint32_t clear_y = height_ - line_height;
    fill_rect(0, clear_y, width_, line_height, bg);
}
```

`scroll_up` 是三步操作：先把所有像素向上移动 `lines` 行（字节级拷贝），然后清空底部空出来的 `line_height` 行像素。之所以用逐字节拷贝而不是 `memcpy`/`memmove`，是因为源和目标都是 `volatile` 指针——标准库的内存操作函数不接受 volatile 参数。

源地址计算是 `buf + pitch * lines`：跳过前 `lines` 行的字节数。移动的字节总数是 `(height_ - lines) * pitch`：剩余所有行的字节数。底部清空用 `fill_rect` 完成，从 `height_ - line_height` 行开始填充。

两个参数的设计值得注意：`lines` 是实际要滚动的像素行数，`line_height` 是底部要清空的区域高度。在控制台场景中，这两个值通常相等（都是字体高度），但分开设计提供了灵活性——比如你想滚动半行然后只清除半行。

## 设计决策

### 决策：identity mapping vs. 专用虚拟地址

**问题**: 帧缓冲区的物理地址映射到虚拟地址空间时，是复用 identity mapping 还是分配一个专用的虚拟地址范围？

**本项目的做法**: 使用 identity mapping——映射后的虚拟地址等于物理地址。

**备选方案**: 分配一个专用的虚拟地址范围（比如 `0xFFFF_C000_0000_0000` 开始的 MMIO 区域），所有 MMIO 设备映射到这个范围内。

**为什么不选备选方案**: identity mapping 在 bootloader 阶段已经部分建立，复用更简单。专用 MMIO 区域需要额外的虚拟地址空间管理器，当前内核的驱动数量不足以证明这个复杂度的必要性。Linux 使用专用 ioremap 区域，但 Linux 需要管理成百上千个 MMIO 设备。

**如果要扩展**: 当驱动数量增多后，应该引入一个简单的 MMIO 虚拟地址分配器，把所有设备映射到 higher-half 的专用区域。

### 决策：逐字节拷贝 vs. SIMD 优化

**问题**: scroll_up 中使用逐字节拷贝，性能较差。

**本项目的做法**: 简单的 for 循环逐字节拷贝。

**备选方案**: 使用 SSE/AVX 指令进行批量拷贝，或者通过一个 non-volatile 临时缓冲区间接使用 memcpy。

**为什么不选备选方案**: 当前文本滚动的频率很低（每次只滚一行，16 像素高），性能瓶颈不存在。volatile 约束使得直接使用 SIMD 需要额外的类型转换。等到真正需要高性能渲染（比如 GUI 窗口拖拽）时再优化。

## 扩展方向

- **双缓冲（Double Buffering）**：分配一块与帧缓冲区同样大小的后备缓冲区，所有绘制操作先写到后备缓冲区，完成后一次性拷贝到前端。难度：⭐⭐
- **硬件加速 BitBLT**：如果模拟器支持 Bochs VBE Extensions 的虚拟显存操作，可以利用 `VBE_DISPI_INDEX_WIN` 等寄存器加速矩形填充和位块传输。难度：⭐⭐⭐
- **多分辨率支持**：从 BootInfo 动态读取分辨率和 bpp，支持 640x480、800x600、1920x1080 等多种模式。难度：⭐

## 参考资料

- Intel SDM: Vol.3A §4.5 (4-Level Paging) — 2MB page PDE format (Table 4-18), 1GB page PDPTE format (Table 4-16)
- Intel SDM: Vol.3A §4.1.4 — 1GB page support via CPUID.80000001H:EDX bit 26
- OSDev Wiki: [VESA BIOS Extensions](https://wiki.osdev.org/VBE) — ModeInfoBlock, linear framebuffer, PhysBasePtr
- Linux: fbdev 子系统使用 `ioremap()` 将帧缓冲区映射到专用虚拟地址范围，支持多种像素格式和硬件加速
