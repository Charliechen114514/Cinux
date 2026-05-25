---
title: 全局构造函数未调用
---

# 006-02: Object 库模式下全局构造函数未被调用的问题

## 问题概述

在将 CMake 构建系统从静态库改为 object 库后，内核串口驱动卡在 `!is_tx_ready()` 无限循环。调试发现 `g_serial` 对象的 `base_port` 成员为 0，而不是预期的 `0x3F8`。

### 表现症状

```log
# debug.log 输出
OPLUUHT...T...T...
T 00 00 00 00  (base_port=0x0, LSR_port=0x5, LSR=0x0)
```

- `OPLUUH`: bootloader 的调试输出
- `T`: 串口 putc() 的 timeout 标记
- `00 00 00 00`: base_port 高/低字节、LSR 值均为 0

## 调试过程

### 1. 初步排查

首先检查 `Serial` 构造函数是否被调用。在构造函数中添加了调试输出：

```cpp
Serial::Serial(uint16_t port) : base_port(port) {
    // 调试输出 'C' 和 port 值
    __asm__ volatile("movb $0x43, %%al; outb %%al, $0xE9" ::: "eax");  // 'C'
    // ... 输出 port 值
    init();
}
```

**结果：** 没有看到 `C` 字符，说明构造函数根本没有被调用！

### 2. 检查 init_array

```bash
$ objdump -s -j .init_array build/kernel/mini/mini_kernel

Contents of section .init_array:
 ffffffff80022378 0d050280 ffffffff                    ........
```

init_array 中确实包含了构造函数指针 `0xffffffff8002050d`。

```bash
$ nm build/kernel/mini/mini_kernel | grep "_GLOBAL__sub_I"

ffffffff8002050d t _GLOBAL__sub_I__ZN5cinux4mini6serial6SerialC2Et
```

符号存在，但没被调用。

### 3. 检查 _init_global_ctors

在 `_init_global_ctors()` 中添加调试输出：

```cpp
void _init_global_ctors() {
    __asm__ volatile("movb $0x7B, %%al; outb %%al, $0xE9" ::: "memory");  // '{'
    // ...
}
```

**结果：** 没有看到 `{` 字符，说明 `_init_global_ctors()` 本身没有被调用！

### 4. 检查 boot.S

boot.S 的 `_start` 函数应该输出 `1234`：

```asm
_start:
    movb $0x31, %al    # '1'
    outb %al, $0xE9
    # ...
    call _init_global_ctors
    movb $0x34, %al    # '4'
    outb %al, $0xE9
```

**结果：** debug.log 中没有 `1234`，说明 `_start` 没有被执行！

### 5. 根本原因发现

检查内核 entry point 和 bootloader 跳转地址：

```bash
# 内核 entry point
$ readelf -h build/kernel/mini/mini_kernel | grep Entry
Entry point address: 0xffffffff8002012a

# bootloader 跳转地址 (stage2.S:304)
movq $0xFFFFFFFF80020000, %rax  # 硬编码地址！
```

**发现问题：** bootloader 跳转到 `0xFFFFFFFF80020000`，但 entry point 在 `0xFFFFFFFF8002012a`，相差 `0x12a` 字节！

检查 `0x80020000` 处是什么：

```bash
$ objdump -d build/kernel/mini/mini_kernel | grep "80020000:"
ffffffff80020000:  55  push   %rbp
```

这是 `mini_kernel_main` 的开头（`0x55` = push rbp），也就是 bootloader 输出的 `UU` 的来源！

## 问题分析

### 为什么之前没问题？

使用静态库时，链接器将所有对象文件合并，`_start` 符号可能恰好被放在了开头位置。

### 为什么 object 库出问题？

Object 库模式下，CMake 的 `target_sources()` 添加对象文件的顺序可能影响了链接器排列符号的顺序。编译器优化也可能重新排列了 `.text` section 中的函数。

### 为什么看起来"能运行"？

Bootloader 跳转到的 `0x80020000` 处恰好是 `mini_kernel_main` 的开头，所以 kprintf 能被调用。但由于全局构造函数没有执行，`g_serial` 对象是未初始化的（BSS 段被清零），导致 `base_port = 0`。

## 解决方案

### 修改 1: 链接器脚本 (linker.ld)

确保 `_start` 在 `.text` section 的开头：

```ld
SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start 必须在最前面！ */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    // ...
}
```

### 修改 2: boot.S

将 `_start` 放入专用的 `.text.start` section：

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    cli
    // ...
```

### 验证修复

```bash
$ readelf -h build/kernel/mini/mini_kernel | grep Entry
Entry point address: 0xffffffff80020000  # ✅ 现在对齐了

$ objdump -d build/kernel/mini/mini_kernel | head -5
ffffffff80020000 <_start>:
ffffffff80020000:  fa  cli
ffffffff80020001:  b0 31  mov $0x31,%al
```

## 调试输出对比

### 修复前

```
OPLUUHT...T...T...
T 00 00 00 00  (timeout: base_port=0)
```

### 修复后

```
OPLUUH1234{E...}C...
        ^^^^  boot.S _start 输出
            ^ _init_global_ctors
              ^ 调用构造函数
                ^ Serial 构造函数
```

## 关键经验总结

### 1. Bootloader 与内核的约定

Bootloader 需要知道内核的正确 entry point。硬编码地址是脆弱的设计：

```asm
# ❌ 脆弱：硬编码地址
movq $0xFFFFFFFF80020000, %rax

# ✓ 更好：从 ELF header 读取
# bootloader 应该解析 ELF 的 e_entry 字段
```

但对于 flat binary（无 ELF 头），需要确保内核代码布局与 bootloader 期望一致。

### 2. Object 库的链接顺序

CMake 的 object 库使用 `target_sources()` 添加对象文件：

```cmake
add_library(mini_kernel_common OBJECT ...)
target_sources(mini_kernel PRIVATE $<TARGET_OBJECTS:mini_kernel_common>)
```

这种模式下，对象文件的顺序可能影响最终的符号排列。

### 3. 调试技巧总结

| 技巧 | 用途 | 示例 |
|------|------|------|
| debugcon (0xE9) | 早期调试，无依赖 | `outb %al, $0xE9` |
| 检查 entry point | 验证入口地址 | `readelf -h kernel \| grep Entry` |
| 检查 init_array | 验证构造函数 | `objdump -s -j .init_array` |
| 检查符号位置 | 验证代码布局 | `objdump -d kernel \| head` |
| flat binary 对比 | 验证实际加载内容 | `xxd kernel.bin \| head` |

### 4. 问题排查流程

```
现象：全局变量未初始化
    ↓
构造函数被调用了吗？
    ↓ 否 → 检查 init_array 和 _init_global_ctors
    ↓ 是 → 检查构造函数本身
    ↓
_init_global_ctors 被调用了吗？
    ↓ 否 → 检查 _start 是否被执行
    ↓ 是 → 检查 init_array 内容
    ↓
_start 被执行了吗？
    ↓ 否 → 检查 bootloader 跳转地址 vs entry point
    ↓ 是 → 问题在 _start 内部
```

## 相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/mini/linker.ld` | 添加 `*(.text.start)` |
| `kernel/mini/arch/x86_64/boot.S` | 改用 `.section .text.start` |
| `kernel/mini/arch/x86_64/crt_stub.cpp` | 添加调试输出 |
| `kernel/mini/driver/serial.cpp` | 添加调试输出 |

## 参考资料

- [LD Documentation: SECTIONS](https://sourceware.org/binutils/docs/ld/SECTIONS.html)
- [OSDev Wiki: Linker Scripts](https://wiki.osdev.org/Linker_Scripts)
- [ELF Specification: Program Header](https://refspecs.linuxfoundation.org/elf/elf.pdf)
