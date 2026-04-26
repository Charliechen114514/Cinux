# Bootloader

> 里程碑: `001_boot_real_mode` `002_boot_gdt_protected` `003_boot_long_mode` `004_boot_load_mini_kernel`

## 功能概述

Cinux 采用三阶段引导：MBR (实模式) → Stage2 (实模式→保护模式→长模式) → 小内核。Bootloader 负责 A20 开启、VESA 图形模式设置、E820 内存枚举、临时页表构建，并将 `BootInfo` 结构体传递给小内核。

## 引导流程

### Stage 1 — MBR (`boot/mbr.S`)
- `.code16` 规范化 CS，清零段寄存器，保存启动盘号
- `print_string`: `lodsb` + BIOS `INT $0x10 AH=0x0E` 循环输出
- DAP 结构 + `INT $0x13 AH=0x42` 扩展磁盘读，加载 stage2 到 `0x8000`
- `ljmp $0, $0x8000` 跳转 stage2

### Stage 2 — 实模式操作 (`boot/stage2.S`, `boot/common/boot.S`)
- 开启 A20: `INT $0x15 AX=0x2401`
- VESA VBE 模式设置: `0x144` (1024x768x32bpp linear framebuffer)
  - 获取 `PhysBasePtr`、`BytesPerScanLine`、`XResolution`、`YResolution`、`BitsPerPixel`
  - 参数打包到 `0x6400` 供后续填入 BootInfo
- E820 内存枚举: `INT $0x15 AX=0xE820`，最多 32 条 `MemoryMapEntry` 存入 `0x5000`
- 从磁盘加载小内核 ELF header 及完整小内核到 `0x20000`

### Stage 2 — 保护模式 & 长模式
- 保护模式: GDT (null/code32/data32) + `lgdt` + `cr0.PE` + `ljmp $0x08`
- 长模式: 临时页表 (PML4→PDPT→PD, 2MB 大页映射 0-8MB) + PAE + EFER.LME + `cr0.PG`
- GDT 追加 64-bit 代码段 (`0x00AF9A000000FFFF`)
- 填充 `BootInfo` 到 `0x7000`，跳转小内核

## 关键数据结构

### BootInfo (`boot/boot_info.h`)
```c
typedef struct {
    uint64_t base, length;
    uint32_t type, _pad;
} MemoryMapEntry;

typedef struct {
    uint64_t entry_point, kernel_phys_base, kernel_size;
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t mmap_count;
    MemoryMapEntry mmap[32];
} BootInfo;
```

### GDT 表项
- `code32`: `0x00CF9A000000FFFF`
- `data32`: `0x00CF92000000FFFF`
- `code64`: `0x00AF9A000000FFFF` (L=1, D=0)

## 调试标记
- `P` (`0x50`) → QEMU debugcon: 确认进入保护模式
- `L` (`0x4C`) → QEMU debugcon: 确认进入长模式
- `J` (`0x4A`) → QEMU debugcon: 确认即将跳转小内核

## 磁盘布局 (`scripts/build_image.sh`)
| 扇区 | 内容 |
|------|------|
| 0 | MBR (`mbr.S`) |
| 1-15 | Stage2 (`stage2.S`) |
| 16+ | 小内核 (`mini.bin`, flat binary from `objcopy -O binary`) |

## 源码位置
- `boot/mbr.S` — MBR 第一阶段
- `boot/stage2.S` — Stage2 入口
- `boot/common/boot.S` — 内核加载
- `boot/common/long_mode.S` — 长模式切换
- `boot/common/serial.S` — 调试串口
- `boot/boot_info.h` — BootInfo 结构体定义
