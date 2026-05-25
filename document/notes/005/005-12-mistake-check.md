---
title: 页表映射调试
---

# 005 - 页表映射与链接脚本调试排查指南

## 问题概述

在实现 higher-half 内核映射时，遇到了两个关键问题导致内核无法正常启动：

1. **页表 PDPT 索引计算错误**
2. **链接器脚本 LMA 计算错误**

这两个问题都会导致内核在跳转到 higher-half 地址时发生页面错误或访问错误数据。

---

## 问题 1: 页表 PDPT 索引计算错误

### 症状

- 输出停留在 `OPL` 后，没有后续输出
- 使用 identity mapping (0x20000) 可以正常执行
- 使用 higher-half mapping (0xFFFFFFFF80020000) 时发生页面错误

### 根本原因

在 x86-64 分页机制中，虚拟地址 `0xFFFFFFFF80000000` 的索引计算错误：

```
虚拟地址: 0xFFFFFFFF80000000
二进制:   1111111111111111111111111111111110000000000000000000000000000000
          ^^^^^^^^^ bits 47:39 (PML4 索引)
                     ^^^^^^^^ bits 38:30 (PDPT 索引)
```

正确计算：
- **PML4 索引** (bits 47:39) = `0x1FF` = **511** ✓
- **PDPT 索引** (bits 38:30) = `0x1FE` = **510** ← 这里容易写错！
- **PD 索引** (bits 29:21) = `0x000` = **0** ✓

### 错误代码

```assembly
// 错误：使用 PDPT[511]
movl $PD_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PDPT_PHYS_ADDR + (511 * 8)  // ← 错误！应该是 510
movl $0, PDPT_PHYS_ADDR + (511 * 8) + 4
```

### 正确代码

```assembly
// 正确：使用 PDPT[510]
movl $PD_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // ← 510，不是 511
movl $0, PDPT_PHYS_ADDR + (510 * 8) + 4
```

### 为什么是 510？

`0x80000000` 的 bit 30 = 0，所以：

```
bits 38:30 = 0b111111110 = 0x1FE = 510
```

如果使用 PDPT[511]，CPU 会查找一个不存在的页表项（not present），触发页面错误。

### 验证方法

添加调试代码输出页表项：

```assembly
// 在进入 long mode 后，读取并输出页表项
movq %cr3, %rax                 // 获取 PML4 地址
movq 0xFF8(%rax), %rbx          // 读取 PML4[511]
// 输出 %rbx 的值，应该是 0x2003

movq $0x2000, %rax              // PDPT 地址
movq 0xFF8(%rax), %rbx          // 读取 PDPT[511] - 应该是 0！
movq 0xFF0(%rax), %rbx          // 读取 PDPT[510] - 应该是 0x3003
```

### 预防措施

1. **使用宏定义**：定义清晰的视频地址分解宏

```assembly
// x86-64 higher-half 页表索引
.set HH_PML4_INDEX,    511  // 0xFFFFFFFF80000000 的 PML4 索引
.set HH_PDPT_INDEX,    510  // ← 注意：是 510，不是 511！
.set HH_PD_INDEX,      0    // PD 索引
```

2. **添加注释**：在页表设置代码中添加详细注释

```assembly
// PDPT[510] -> PD
// 0x80000000 的 bit 30 = 0，所以 PDPT 索引是 510
// 计算：bits 38:30 = 0b111111110 = 0x1FE = 510
```

---

## 问题 2: 链接器脚本 LMA 计算错误

### 症状

- `__init_array` 中的构造函数指针是 `0xffffffff` (垃圾值)
- 调用全局构造函数时崩溃
- 数据段内容不正确

### 根本原因

使用 `SIZEOF()` 累加计算 LMA 时，**没有考虑段间对齐填充**。

#### 错误的链接器脚本

```ld
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {  // ← 错误！
        *(.data .data.*)
    }

    .init_array : AT(KERNEL_PHYS_BASE + SIZEOF(.text) + SIZEOF(.data)) {  // ← 错误！
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }
}
```

#### 问题分析

```
内存布局（VMA）:
0xFFFFFFFF80020000  .text 开始
0xFFFFFFFF80020xxx  .text 结束（可能有对齐填充）
0xFFFFFFFF80021xxx  .data 开始  ← 对齐填充导致偏移！
0xFFFFFFFF80021yyy  .data 结束
0xFFFFFFFF80021zzz  .init_array 开始

SIZEOF(.text) 只返回 .text 段内容的大小，不包含对齐填充
所以 KERNEL_PHYS_BASE + SIZEOF(.text) 计算出的 LMA 位置错误！
```

#### 正确的链接器脚本

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
}
```

### 为什么 `ADDR() - KERNEL_Virt_BASE` 有效？

```
LMA = VMA - KERNEL_Virt_BASE

对于任何地址：
- VMA = 0xFFFFFFFF80020000
- LMA = 0xFFFFFFFF80020000 - 0xFFFFFFFF80000000 = 0x20000

无论段间有多少对齐填充，这个关系始终成立！
```

### 验证方法

使用 `readelf` 检查段布局：

```bash
# 查看 ELF 段的 LMA (Load Address)
readelf -l build/kernel/mini/mini_kernel.elf | grep -A 20 "Program Headers"

# 输出示例：
# LOAD           0x0000000000002000 0xffffffff80020000 0xffffffff80020000
#                0x0000000000001000 0x0000000000001000  R E
```

关键检查项：
- **VMA** (虚拟地址) 应该是 higher-half 地址
- **LMA** (加载地址) 应该是物理地址（0x20000 起始）
- **File Size** 和 **Mem Size** 应该匹配（对于非 .bss 段）

### objcopy 验证

```bash
# 检查 raw binary 的内容
xxd build/kernel/mini/mini_kernel.bin | head -20

# 应该看到内核代码的正确内容
# 第一个字节应该是 0xFA (cli 指令)
```

---

## 调试工具和方法

### 1. 使用 QEMU debugcon

最简单的调试方法：

```bash
qemu-system-x86_64 -drive file=cinux.img,format=raw -debugcon file:debug.log
```

在代码中添加调试输出：

```assembly
// 输出单个字符
movb $0x41, %al    // 'A'
outb %al, $0xE9    // 写入 debugcon 端口
```

```cpp
// C/C++ 中使用
__asm__ volatile("movb $0x42, %%al; outb %%al, $0xE9" ::: "eax");  // 'B'
```

### 2. 页表验证代码

在进入 long mode 后添加页表验证：

```assembly
// 读取 PML4[511]
movq %cr3, %rax
movq 0xFF8(%rax), %rbx
// 输出 %rbx，应该是 0x0000000000002003

// 读取 PDPT[510]
movq $0x2000, %rax
movq 0xFF0(%rax), %rbx  // PDPT[510] 在偏移 510*8 = 0xFF0
// 输出 %rbx，应该是 0x0000000000003003

// 读取 PD[0]
movq $0x3000, %rax
movq (%rax), %rbx
// 输出 %rbx，应该是 0x0000000000000083
```

### 3. GDB 调试

```bash
# 启动 QEMU with GDB server
qemu-system-x86_64 -drive file=cinux.img,format=raw -s -S

# 在另一个终端连接 GDB
gdb build/kernel/mini/mini_kernel.elf
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
```

### 4. 检查符号表

```bash
# 查看内核符号地址
nm build/kernel/mini/mini_kernel.elf | grep -E "__init_array|_start"

# 预期输出：
# ffffffff80020000 T _start
# ffffffff80020xxx A __init_array_start
# ffffffff80020xxx A __init_array_end
```

---

## 故障排查流程图

```
内核启动失败
    │
    ├─> 输出停留在 "O" 之前
    │   └─> 检查磁盘加载、MBR 执行
    │
    ├─> 输出停留在 "OP" 之间
    │   └─> 检查保护模式切换、GDT 加载
    │
    ├─> 输出停留在 "PL" 之间
    │   └─> 检查 long mode 切换、PAE 启用
    │
    ├─> 输出有 "L" 但没有后续
    │   └─> 检查页表设置
    │       │
    │       ├─> 验证 PML4[511] = 0x2003
    │       ├─> 验证 PDPT[510] = 0x3003  ← 注意是 510！
    │       └─> 验证 PD[0] = 0x83
    │
    ├─> 输出有 "1" 但没有 "2"
    │   └─> 检查栈指针设置
    │
    ├─> 输出有 "2" 但没有 "3"
    │   └─> 检查 BSS 清零
    │
    ├─> 输出有 "3" 但没有 "4"
    │   └─> 检查 __init_array
    │       │
    │       ├─> 验证 __init_array_start < __init_array_end
    │       ├─> 验证构造函数指针有效（不是 0 或 0xffffffff）
    │       └─> 检查链接器脚本 LMA 计算
    │
    └─> 输出有 "4" 但没有内核输出
        └─> 检查 mini_kernel_main 入口
```

---

## 常见错误和解决方案

### 错误 1: 页表项高 32 位未清零

**症状**：访问 higher-half 地址时页面错误

**原因**：在 32 位代码中设置 64 位页表项时，只写入了低 32 位

**解决**：
```assembly
// 正确：写入低 32 位和高 32 位
movl %eax, PML4_PHYS_ADDR + (511 * 8)  // 低 32 位
movl $0, PML4_PHYS_ADDR + (511 * 8) + 4  // 高 32 位 = 0
```

### 错误 2: 忘记添加 higher-half 映射

**症状**：identity mapping 工作但 higher-half 不工作

**原因**：只设置了 PML4[0]，忘记设置 PML4[511]

**解决**：
```assembly
// Identity mapping (PML4[0])
movl $PDPT_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PML4_PHYS_ADDR  // PML4[0]

// Higher-half mapping (PML4[511])
movl $PDPT_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511]
movl $0, PML4_PHYS_ADDR + (511 * 8) + 4
```

### 错误 3: 跳转到物理地址而不是虚拟地址

**症状**：跳转后立即崩溃或执行错误代码

**原因**：在 higher-half 代码中使用了物理地址

**解决**：
```cpp
// 错误
void (*ctor)() = (void (*)())func_phys;  // 物理地址
ctor();

// 正确
void (*ctor)() = (void (*)())func_hh;    // higher-half 地址
ctor();
```

### 错误 4: 链接器脚本 AT() 参数错误

**症状**：数据内容错误，构造函数指针是垃圾值

**原因**：使用 SIZEOF() 累加计算 LMA，没有考虑对齐填充

**解决**：使用 `ADDR(section) - KERNEL_Virt_BASE`

---

## 最佳实践

### 1. 页表设置

```assembly
// 使用清晰的常量定义
.set PML4_PHYS_ADDR, 0x1000
.set PDPT_PHYS_ADDR, 0x2000
.set PD_PHYS_ADDR,   0x3000

.set HH_PML4_INDEX,  511
.set HH_PDPT_INDEX,  510  // ← 注意！
.set HH_PD_INDEX,    0

// 添加详细注释
// PML4[511] -> PDPT
// 0xFFFFFFFF80000000 的 bits 47:39 = 0x1FF = 511
movl $PDPT_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PML4_PHYS_ADDR + (HH_PML4_INDEX * 8)
movl $0, PML4_PHYS_ADDR + (HH_PML4_INDEX * 8) + 4

// PDPT[510] -> PD
// 0xFFFFFFFF80000000 的 bits 38:30 = 0x1FE = 510
// 注意：bit 30 = 0，所以是 510 不是 511！
movl $PD_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PDPT_PHYS_ADDR + (HH_PDPT_INDEX * 8)
movl $0, PDPT_PHYS_ADDR + (HH_PDPT_INDEX * 8) + 4
```

### 2. 链接器脚本

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    /* 使用 ADDR() - KERNEL_Virt_BASE 计算 LMA */
    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text .text.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        __bss_end = .;
    }
}
```

### 3. 调试输出策略

```assembly
// 使用十六进制输出，便于调试
// 输出格式：[高字节] [低字节]
movq %rax, %rcx
shrq $24, %rcx
outb %cl, $DEBUGCON_PORT

movq %rax, %rcx
shrq $16, %rcx
outb %cl, $DEBUGCON_PORT

movq %rax, %rcx
shrq $8, %rcx
outb %cl, $DEBUGCON_PORT

movb %al, %cl
outb %cl, $DEBUGCON_PORT
```

---

## 参考资源

### x86-64 分页机制

- Intel SDM Vol. 3A, Chapter 4: Paging
- AMD64 Architecture Programmer's Manual Vol. 2, Chapter 5: Page Tables

### 页表项格式

```
+-------+------+---+---+---+---+---+---+
| NX    | Avail | G | 0 | D | A | PCD | PWT |
| (bit 63) | 62:52 | 51 | 50 | 49 | 48 | 47 |
+-------+------+---+---+---+---+---+---+
| Available |  Reserved  |  Physical Address    |
| 51:9      |    8       |       39:12           |
+-----------+------------+----------------------+
| Available | G | PS | 0 | A | D | PAT | U | W | P |
| 11:9      | 8 | 7  | 6 | 5 | 4 |  3   | 2 | 1 | 0 |
+-----------+---+---+---+---+---+------+---+---+---+
```

### 虚拟地址分解（48-bit）

```
+--------+--------+--------+----------+
| PML4   | PDPT   | PD     | Offset  |
| 47:39  | 38:30  | 29:21  | 20:0     |
| 9 bits | 9 bits | 9 bits | 21 bits  |
+--------+--------+--------+----------+
```

---

## 总结

本次调试过程中的两个关键问题：

1. **页表 PDPT 索引错误**：0xFFFFFFFF80000000 的 PDPT 索引是 510，不是 511
2. **链接器脚本 LMA 错误**：使用 `ADDR() - KERNEL_Virt_BASE` 而不是 `SIZEOF()` 累加

这两个问题都源于对 x86-64 架构细节和链接器行为的理解不足。通过系统的调试方法和工具（QEMU debugcon、GDB、readelf），我们成功定位并解决了问题。

**记住**：
- 页表索引计算要准确，特别是 bit 30 = 0 意味着 PDPT 索引是 510
- LMA 计算要考虑对齐填充，使用 `ADDR()` 而不是 `SIZEOF()`
- 添加详细的调试输出，逐段验证代码执行
