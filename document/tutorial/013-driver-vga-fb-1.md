---
title: 013-driver-vga-fb-1 · VGA 帧缓冲
---

# 013-1 底座：把屏幕点亮——线性帧缓冲区与 MMIO 页表映射

> 标签：framebuffer, VBE, MMIO, 页表映射, 2MB 大页, identity mapping
> 前置：[012 串口驱动与 kprintf](012-driver-serial-3.md)

## 前言

说实话，搞了这么多章，我们的内核到现在还是个"瞎子"——能通过串口打印日志，但 QEMU 窗口里始终是一片漆黑。串口调试当然够用，但每次启动都要另外开一个终端窗口连串口，多少有点寒酸。更重要的是，如果我们最终想让这个内核有图形界面、有窗口管理器，第一步总得先把像素画到屏幕上吧。

这一章我们要做的事情说起来很简单：把 bootloader 通过 VBE 设置好的线性帧缓冲区映射到内核的虚拟地址空间里，然后提供一个能在上面画像素的 Framebuffer 类。但"简单"两个字背后藏着两个真正的坑——帧缓冲区的物理地址可能不在已有页表的映射范围内，而且操作 MMIO 内存的时候编译器随时可能给你"帮倒忙"。

先别急，我们一步一步来。

## 环境说明

我们运行在 QEMU（默认 Bochs VBE 扩展）或真实 Bochs 模拟器上，帧缓冲区的物理地址通常在 0xE0000000 附近（3.5GB 处）。Bootloader 在 tag 003/004 中已经通过 VBE INT 0x10 调用设置了 1024x768x32bpp 的图形模式，帧缓冲区的信息（物理地址、宽高、pitch、bpp）被写入 BootInfo 结构体放在物理地址 0x7000。内核的页表由 bootloader 在进入 long mode 时设置，只映射了前几 MB 的物理内存。

## 第一步：理解 VBE 线性帧缓冲区

在我们开始写代码之前，先搞清楚帧缓冲区到底是个什么东西。VBE 2.0 以后，显卡可以提供一个连续的物理内存区域作为线性帧缓冲区（Linear Framebuffer）。在这块内存里，屏幕上每个像素的颜色被存储为一个固定长度的整数——我们的场景中是 32 位，格式是 0x00RRGGBB（红绿蓝各 8 位，最高字节未使用）。

屏幕上第 x 列、第 y 行的像素在内存中的位置是：

```
帧缓冲区基地址 + y * pitch + x * 4
```

这里 pitch 是每行占用的字节数。你会发现我在这里用的是 pitch 而不是 `width * 4`，这是有原因的——有些显卡会在每行末尾添加填充字节来对齐到某个边界。举个例子，如果屏幕宽度是 1024 像素，理论上每行 4096 字节，但某些硬件可能把 pitch 设成 4096 的倍数来满足对齐要求。所以你必须用 BootInfo 里的 pitch 值，永远不要自己算。这个坑在 OSDev 社区坑了无数人，OSDev Wiki 的 [VESA BIOS Extensions](https://wiki.osdev.org/VBE) 页面专门强调了这一点。

现在问题来了：Bochs 默认把帧缓冲区放在物理地址 0xE0000000，也就是 3.5GB 的位置。我们的 bootloader 在进入 long mode 时设置了一套基本的页表，只映射了前几 MB 的物理内存。所以访问 0xE0000000 之前，必须先在页表中添加对应的映射——否则一写就是 page fault，如果还没有中断处理程序的话就是 triple fault，直接重启。这就是我们需要 `map_mmio()` 函数的原因。

## 第二步：实现 MMIO 页表映射

我们现在要做的是在已有的页表结构中添加新的映射条目。bootloader 在进入 long mode 时已经建好了四级页表（PML4 -> PDPT -> PD -> PT），并且设置了 higher-half 映射，所以 PD 和 PDPT 的虚拟地址是已知的常量。

先来看 paging.hpp 的接口——非常简洁，只有一个函数：

```cpp
namespace cinux::arch {
void map_mmio(uint64_t phys, uint64_t size);
}
```

接口之所以这么简单，是因为我们使用 identity mapping——映射后的虚拟地址就等于物理地址。这意味着调用者不需要知道映射到了哪个虚拟地址，直接用物理地址访问就行。这种设计在内核开发初期很方便，但 Linux 不这么做——Linux 使用专门的 `ioremap()` 函数把 MMIO 区域映射到 higher-half 的专用虚拟地址范围，好处是不会与直接映射区域冲突，也更容易区分"普通内存"和"设备内存"。

接下来看 paging.cpp 的实现。匿名命名空间里定义了一组常量：

```cpp
constexpr uint64_t PD_HUGE_PAGE_FLAGS = 0x83;
constexpr uint64_t PAGE_2MB_SIZE = 0x200000;
constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;
constexpr uint64_t PD_VIRT_ADDR = 0xFFFFFFFF80003000ULL;
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;
constexpr uint32_t PDPT_PD_ENTRY = 0;
```

`0x83` 这个标志值需要特别解释一下。拆开来看是三个 bit 的组合：bit 0 是 Present（页存在）、bit 1 是 Read/Write（可读写）、bit 7 是 Page Size（大页标志）。Page Size 位置 1 告诉 CPU "这不是一个指向下一级页表的指针，而是一个直接映射大页的条目"。Intel SDM Vol.3A 的 Table 4-18 详细描述了 2MB 大页的 PDE 格式：bits 51:21 存放物理地址，低位存放标志位。1GB 大页的 PDPTE 格式在 Table 4-16 中描述，结构类似但地址位是 bits 51:30。

`PDPT_PD_ENTRY = 0` 这个常量是一个安全守卫——PDPT[0] 指向我们的 PD，覆盖了等于把所有低地址的映射全部干掉。这个错误一旦犯了你连 kprintf 都用不了，因为串口的 MMIO 地址也在低地址空间里。

两个辅助函数先看一下：

```cpp
bool has_1gb_pages() {
    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;
}
```

1GB 大页不是所有 x86_64 CPU 都支持的——Intel SDM Vol.3A Section 4.1.4 明确指出，需要通过 CPUID 扩展功能号 0x80000001 的 EDX bit 26 来检查。QEMU 在 TCG 模式下不一定支持 1GB 页，KVM 模式通常支持。所以这个运行时检查是必要的，不能偷懒。

```cpp
void reload_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

重新加载 CR3 会刷新整个 TLB——这是一种比较粗暴的方式，但对于修改 PDPT 条目（每个条目影响 1GB 地址空间）来说，这是唯一可靠的方法。对于 2MB 大页，我们有更精确的 `invlpg` 指令可用。

现在来看 `map_mmio()` 的主体逻辑：

```cpp
void map_mmio(uint64_t phys, uint64_t size) {
    auto* pd = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);
    uint64_t end = phys + size;
```

把 PD 和 PDPT 的虚拟地址转成 `volatile uint64_t*`——这里的 volatile 是必须的，因为页表条目是硬件也在访问的数据结构，编译器绝对不能优化掉对它们的写入操作。如果你忘了加 volatile，编译器在优化级别 `-O2` 以上可能会把连续的两次页表写入合并成一次，或者干脆认为"这个值没被读过"就跳过了——后果就是页表映射根本没建起来。

接下来是第一部分——处理 1GB 以内的物理地址，用 2MB 大页映射：

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

`phys & ~(PAGE_2MB_SIZE - 1)` 把物理地址向下对齐到 2MB 边界——x86 的大页必须对齐到自身大小，这是硬件要求。循环中首先检查目标条目是否为空（`pd[idx] == 0`），这是为了避免覆盖已有映射——比如内核代码段和数据段所在的地址范围已经在 PD 中有映射了，不能覆盖。

每填充一个 PDE 后立刻调用 `invlpg` 刷新对应的 TLB 条目。`invlpg` 的参数是一个虚拟地址，CPU 会 invalidate 包含该地址的 TLB 条目。这种逐条刷新比重新加载 CR3 温和得多——只影响一个 2MB 的 TLB 条目，不会把整个 TLB 全部清空。

一个 1024x768x32bpp 的帧缓冲区大约 3MB（1024 * 768 * 4 = 3,145,728 字节），需要 2 个 2MB 大页。但如果帧缓冲区物理地址在 0xE0000000（3.5GB），就超出了 1GB 的 PD 覆盖范围——这时需要走到第二部分。

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

第二部分处理 1GB 以上的范围，直接在 PDPT 中填 1GB 大页条目。这里有三重安全检查：`n != PDPT_PD_ENTRY` 保证不覆盖 PDPT[0]（指向 PD 的条目），`pdpt[n] == 0` 避免覆盖已有映射，`n < PT_ENTRIES` 防止越界。如果对齐后的起始地址落在 1GB 以内，就强制跳到 1GB 边界——因为第一部分已经处理了 1GB 以内的区域。

修改 PDPT 后必须调用 `reload_cr3()` 刷新全局 TLB，因为 `invlpg` 对于 PDPT 级别的修改可能不够可靠。

## 第三步：实现 Framebuffer 类

页表映射搞定了，现在可以安全地访问帧缓冲区内存了。Framebuffer 类把这块裸内存包装成一套简洁的绘图原语。

先看 `init()` 方法——它是整个帧缓冲区驱动的入口：

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

从 BootInfo 中提取五个参数，用 `pitch * height`（不是 `width * 4 * height`！）计算帧缓冲区总大小，然后调用 `map_mmio()` 映射这片物理内存。映射完成后，由于我们使用 identity mapping，物理地址可以直接当虚拟地址用——最后一行 `reinterpret_cast` 把物理地址转成了 `volatile uint32_t*` 指针。

这里有一个非常容易忽略的细节：`fb_size` 用的是 `pitch_` 而不是 `width_ * 4`。如果碰巧 pitch 等于 `width * 4`，结果一样；但如果 pitch 更大，用 width 计算就会少映射一部分内存，访问行尾的填充区域时直接 page fault。Linux 的 fbdev 子系统也做了同样的事情——在 `fb_info->screen_size` 中使用 `fix.line_length * var.yres`（其中 `fix.line_length` 就是我们的 pitch）来计算总大小。

然后是最基础的 `put_pixel`：

```cpp
void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= width_ || y >= height_)
        return;
    addr_[y * (pitch_ / 4) + x] = argb;
}
```

像素地址计算公式是 `y * (pitch_ / 4) + x`——先把 pitch 从字节数转换成 uint32_t 的个数（除以 4），然后乘以行号加列号。边界检查用 `>=`（坐标是 0-indexed）。超界就静默忽略——这在图形编程中很合理，裁剪是最常见的操作，不应该报错。

接下来看稍微复杂一些的 `scroll_up`：

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

滚动操作是三步：先把所有像素向上移动 `lines` 行（字节级逐字节拷贝），然后清空底部空出来的 `line_height` 行像素。你可能会问为什么不直接用 `memcpy` 或 `memmove`——因为源和目标都是 `volatile` 指针，标准库函数不接受 volatile 参数。我们只能手写逐字节拷贝。

说实话这种逐字节拷贝的性能确实不行——一个 1024x768x32bpp 的帧缓冲区有 3MB 数据，如果要滚动一整行（16 像素），需要拷贝大约 `(768 - 16) * 4096 = 3,082,240` 字节，逐字节拷贝的话就是三百万次循环迭代。但考虑到控制台文本滚动的频率极低（用户打字速度），这完全不是瓶颈。Linux 的 fbcon 实现了三种滚动模式——SCROLL_REDRAW（和我们一样的 memmove）、SCROLL_PAN（通过切换显示起始地址来避免实际拷贝）、SCROLL_WRAP（循环缓冲区）。SerenityOS 的 `GenericFramebufferConsole` 也使用了类似我们的简单 memmove 方式。

## 验证

现在来验证一下。启动 QEMU 后，串口输出应该包含 `[BIG] Framebuffer initialised: 1024x768 32bpp`，QEMU 窗口应该从文字模式变成黑色画面——因为我们调了 `clear()` 把整个帧缓冲区清零了，而 0x00000000 就是纯黑色。

如果你在 init 之后立即写入一个测试像素——比如 `put_pixel(512, 384, 0x00FF0000)`（屏幕正中央一个红色像素），然后用 `get_pixel` 读回来验证：

```
fb.put_pixel(512, 384, 0x00FF0000);
uint32_t val = fb.get_pixel(512, 384);
kprintf("[TEST] pixel readback: 0x%p\n", (uint64_t)val);  // 应该输出 0x0000000000FF0000
```

## 收尾

到这里我们已经完成了帧缓冲区的基础设施——能映射、能画像素、能填矩形、能滚动。但光有像素还不够，下一章我们要解决"怎么在像素上画字"的问题，那就需要一套位图字体引擎了。

## 参考资料

- Intel SDM: Vol.3A Section 4.5 — 4-Level Paging，Table 4-18 (2MB Page PDE)，Table 4-16 (1GB Page PDPTE)
- Intel SDM: Vol.3A Section 4.1.4 — 1GB page support via CPUID.80000001H:EDX bit 26
- OSDev Wiki: [VESA BIOS Extensions](https://wiki.osdev.org/VBE) — ModeInfoBlock 字段说明、线性帧缓冲区
- Linux fbdev: `drivers/video/fbdev/core/fbmem.c` 中的帧缓冲区注册和 `fix.line_length * var.yres` 大小计算
- SerenityOS: `Kernel/Graphics/FramebufferConsole.h` 使用类似的 volatile 指针直接写入线性帧缓冲区
