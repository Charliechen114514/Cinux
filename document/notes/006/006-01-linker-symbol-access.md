---
title: 链接器符号访问
---

# 006-01: C/C++ 访问链接器符号的正确方式

## 问题概述

在实现 PMM 时，需要从 C++ 代码访问链接器脚本中定义的符号（如 `__kernel_size`）。直接声明 `extern uint64_t __kernel_size` 并访问 `__kernel_size` 得到的是 0，而不是预期的值。

## 根本原因

链接器符号（Linker Script Symbols）**不是变量**，它们只是**地址常量**。链接器在符号表中记录这些符号的值，但**不会为它们分配内存**。

### 错误理解

```
┌─────────────────────────────────────────────────────────────┐
│ 错误理解：认为 __kernel_size 是一个变量                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  extern uint64_t __kernel_size;  // 声明为一个外部变量      │
│                                                             │
│  uint64_t size = __kernel_size;     // 读取变量内存内容      │
│                                      ▼                      │
│                                   读到垃圾值/0              │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 正确理解

```
┌─────────────────────────────────────────────────────────────┐
│ 正确理解：__kernel_size 是一个地址常量                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  链接器符号表：                                             │
│    __kernel_size ─────────────────→ 0x42F0 (地址即值)       │
│                                                             │
│  C 代码中：                                                 │
│    extern char __kernel_size;       // 声明符号存在         │
│    uint64_t size = (uint64_t)&__kernel_size;  // 取地址 = 值 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 代码对比

### ❌ 错误写法

```cpp
// 声明为 uint64_t 变量
extern "C" {
    extern uint64_t __kernel_size;
}

// 直接访问 - 读取该地址的内存内容
uint64_t kernel_size = __kernel_size;  // ❌ 读到 0 或垃圾值
```

**为什么会读到 0？**
- 编译器认为 `__kernel_size` 是一个变量，生成代码读取该地址处的内存
- 但链接器没有为这个符号分配内存
- 该地址处可能什么都没有，或包含 0

### ✓ 正确写法

```cpp
// 声明为 char 类型（类型不重要，只要能取地址）
extern "C" {
    extern char __kernel_size;
}

// 取地址得到符号的值
uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);  // ✓
```

**为什么正确？**
- `&__kernel_size` 获取符号的地址
- 对于链接器符号，**地址即值**
- 类型 `char` 只是为了满足语法，实际会转换为 `uint64_t`

## 完整示例

### 链接器脚本 (linker.ld)

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text .text.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* 导出符号给 C/C++ 代码使用 */
    __mini_kernel_end = .;
    PROVIDE(__kernel_size = (__mini_kernel_end - (KERNEL_Virt_BASE + KERNEL_PHYS_BASE)));
}
```

### C++ 代码 (pmm.cpp)

```cpp
// 正确声明 - 使用 char 类型
extern "C" {
    extern char __kernel_size;     // 内核大小
    extern char __mini_kernel_end; // 内核结束地址
    extern char __bss_start;       // BSS 起始
    extern char __bss_end;         // BSS 结束
}

void init(const BootInfo* info) {
    // 正确访问 - 取地址
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    uint64_t bss_start  = reinterpret_cast<uint64_t>(&__bss_start);
    uint64_t bss_end    = reinterpret_cast<uint64_t>(&__bss_end);

    kprintf("Kernel size: %u bytes\n", kernel_size);
    kprintf("BSS: 0x%x - 0x%x\n", bss_start, bss_end);
}
```

## 类型选择

虽然类型不影响取地址的结果，但有以下约定：

| 用途 | 推荐类型 | 原因 |
|------|---------|------|
| 大小值 | `char` 或 `unsigned char` | 最小类型，语义清晰 |
| 地址 | `char` 或 `void` | 地址就是字节偏移 |
| 数组边界 | `char[]` | 表示指向某个位置 |

**常见模式：**
```cpp
extern char __kernel_size;        // 大小值
extern char __bss_start[];        // 数组起始（等价于指针）
extern char __mini_kernel_end;    // 位置标记
```

## 调试验证

### 1. 检查符号表

```bash
nm kernel/mini/mini_kernel | grep __kernel_size
# 输出: 00000000000042f0 A __kernel_size
#       ^^^^^^^^^^^^^^^^ 值
#                        ^ 绝对符号 (A)
```

### 2. 验证访问方式

```cpp
// 添加调试输出
extern char __kernel_size;

kprintf("&__kernel_size = 0x%x\n", &__kernel_size);  // 应该输出 0x42f0
kprintf("__kernel_size  = 0x%x\n", *__kernel_size);  // 可能是垃圾值，不要这样用
```

### 3. 使用 nm 确认符号类型

```bash
nm -A build/kernel/mini/mini_kernel | grep -E "__kernel_size|__bss"

# 预期输出：
# build/kernel/mini/mini_kernel:00000000000042f0 A __kernel_size
# build/kernel/mini/mini_kernel:0000000000002200 B __bss_start
# build/kernel/mini/mini_kernel:0000000000004220 B __bss_end
#                                               ^ BSS 段符号 (B)
```

## 常见链接器符号

| 符号 | 类型 | 用途 | 访问方式 |
|------|------|------|----------|
| `__bss_start` | B | BSS 段起始 | `(uintptr_t)&__bss_start` |
| `__bss_end` | B | BSS 段结束 | `(uintptr_t)&__bss_end` |
| `__kernel_size` | A | 计算得出的大小 | `(uintptr_t)&__kernel_size` |
| `__init_array_start` | A | 构造函数数组起始 | `(uintptr_t)&__init_array_start` |

## 最佳实践

### 1. 统一的宏定义

```cpp
// kernel/common/linker_symbols.h
#pragma once

extern "C" {
    extern char __kernel_size;
    extern char __bss_start[];
    extern char __bss_end[];
    extern char __mini_kernel_end;
}

// 辅助宏
#define LINKER_SYMBOL_VALUE(sym) (reinterpret_cast<uint64_t>(&(sym)))
#define LINKER_SYMBOL_ADDR(sym)   (reinterpret_cast<uint64_t>(&(sym)))
```

### 2. 类型安全包装

```cpp
namespace cinux {
namespace linker {

inline uint64_t kernel_size() {
    extern char __kernel_size;
    return reinterpret_cast<uint64_t>(&__kernel_size);
}

inline uint64_t bss_start() {
    extern char __bss_start;
    return reinterpret_cast<uint64_t>(&__bss_start);
}

inline uint64_t bss_end() {
    extern char __bss_end;
    return reinterpret_cast<uint64_t>(&__bss_end);
}

} // namespace linker
} // namespace cinux
```

### 3. 链接器脚本注释

```ld
/*
 * Linker symbols exported to C/C++ code.
 *
 * IMPORTANT: In C++, access these symbols using &symbol, not symbol directly.
 * Example: uint64_t size = (uint64_t)&__kernel_size;
 */
__kernel_size = (__mini_kernel_end - KERNEL_Virt_BASE - KERNEL_PHYS_BASE);
```

## 故障排查

### 症状：读到 0

**可能原因：**
1. 符号未导出（缺少 `PROVIDE` 或未赋值）
2. 使用了直接访问而非取地址
3. 链接时未包含正确的 .o 文件

**检查：**
```bash
# 检查符号是否存在
nm your_kernel.elf | grep your_symbol

# 检查代码中是否使用 &
grep "&your_symbol" your_code.cpp
```

### 症状：链接错误 undefined reference

**可能原因：**
1. 声明但未定义（linker.ld 中未定义）
2. `extern "C"` 缺失（C++ 中 name mangling）

**解决：**
```cpp
// 确保 extern "C"
extern "C" {
    extern char __your_symbol;
}
```

## 相关资源

- [LD Documentation: Symbol Definitions](https://sourceware.org/binutils/docs/ld/Simple-Assignments.html)
- [OSDev Wiki: Linker Scripts](https://wiki.osdev.org/Linker_Scripts)
- [GCC Documentation: Asm Labels](https://gcc.gnu.org/onlinedocs/gcc/Asm-Labels.html)

## 总结

| 要点 | 说明 |
|------|------|
| 链接器符号是地址常量 | 不是变量，没有内存分配 |
| 使用 `&symbol` 获取值 | 地址即值 |
| 声明为 `char` 类型 | 类型不重要，能取地址即可 |
| 使用 `extern "C"` | 避免 C++ name mangling |
| 用 `PROVIDE` 导出 | 确保符号在符号表中可见 |

**记住：** 对于链接器符号，`&symbol` 才是你想要的值，`symbol` 本身可能指向未定义的内存。
