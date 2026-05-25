---
title: BootInfo 参数破坏
---

# 🔥 BootInfo 参数被破坏：从"神秘地址"到"BSS 清除"

## 一、问题现象

**症状**：在 `mini_kernel_main` 中访问 `boot_info` 时，指针值是错误的 `0xffffffff80022188`，而不是预期的 `0x7000`。

```cpp
// kernel/mini/main.cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)boot_info_addr;

    // 调试显示 boot_info = 0xffffffff80022188 ❌
    // 预期应该是 boot_info = 0x7000
}
```

**调试输出**：
```
OPLJ12R70003G4D[0000002A00000000]
```

- `R7000` → Bootloader 传递的 `%rdi = 0x7000` ✅ 正确！
- `D[0000002A00000000]` → `__boot_info_ptr = 0x2a00000000` ❌ 错误！

---

## 二、初步排查

### 1. 确认 Bootloader 传递正确

在 `boot.S` 的 `_start` 入口处添加调试输出：

```asm
/* Debug: Output %rdi lower 16 bits */
movl %edi, %eax
shrl $12, %eax    /* Get bits 15-12 */
andl $0xF, %eax
addb $'0', %al
outb %al, $0xE9
... (重复输出 4 个十六进制数字)
```

**输出**：`R7000` → `%rdi = 0x7000` ✅

**结论**：Bootloader 正确传递了 `BootInfo* = 0x7000`，问题在内核内部。

### 2. 检查参数保存和恢复

```asm
/* boot.S */
_start:
    cli
    movb $0x31, %al        /* '1' */
    outb %al, $0xE9

    /* Setup stack */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    movb $0x32, %al        /* '2' */
    outb %al, $0xE9

    /* Save BootInfo pointer BEFORE clearing BSS */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi   /* ❌ 这里破坏了 %rdi！ */
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb                 /* 清除 BSS */

    movb $0x33, %al        /* '3' */
    outb %al, $0xE9

    call _init_global_ctors

    movb $0x34, %al        /* '4' */
    outb %al, $0xE9

    /* Call C++ main */
    movq __boot_info_ptr, %rdi
    call mini_kernel_main
```

---

## 三、真相大白：BSS 清除破坏了参数

### 问题代码

```asm
/* Save BootInfo pointer BEFORE clearing BSS */
movq %rdi, __boot_info_ptr    /* 保存 %rdi = 0x7000 */

/* Clear BSS section */
movq $__bss_start, %rdi       /* ❌ %rdi 被覆盖！ */
```

### 执行流程分析

```
1. bootloader → _start: %rdi = 0x7000 ✅
2. movq %rdi, __boot_info_ptr → __boot_info_ptr = 0x7000 ✅
3. movq $__bss_start, %rdi → %rdi = __bss_start 的地址
4. rep stosb → 清除 BSS，可能影响 __boot_info_ptr（如果在 .bss 段）
5. movq __boot_info_ptr, %rdi → %rdi = 被破坏的值
```

### 为什么会显示 `0x2a00000000`

```
0x2a00000000 = 42 << 32
```

这个值与全局变量 `global_construction_count = 42` 相关，说明存在内存覆盖问题（下一篇笔记详细分析）。

---

## 四、修复方案

### 方案 1：先保存到另一个寄存器（临时方案）

```asm
/* Save BootInfo pointer to a safe register */
movq %rdi, %r11              /* 保存到 %r11 */

/* Clear BSS section (destroys %rdi) */
movq $__bss_start, %rdi
movq $__bss_end, %rcx
subq %rdi, %rcx
xorq %rax, %rax
rep stosb

/* Restore from safe register */
movq %r11, __boot_info_ptr   /* 从 %r11 恢复 */
```

### 方案 2：直接在清除前保存（最终方案）

```asm
/* Save BootInfo pointer BEFORE clearing BSS */
movq %rdi, __boot_info_ptr

/* Clear BSS section */
movq $__bss_start, %rdi
...
```

但还有一个问题：`__boot_info_ptr` 在 `.bss` 段中，会被 `rep stosb` 清除！

---

## 五、更深层的问题：.bss 段位置

### 初始代码（有问题）

```asm
/* boot.S */
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

### 问题

1. `__boot_info_ptr` 在 `.bss` 段中
2. BSS 清除会把 `__bss_start` 到 `__bss_end` 的所有内容清零
3. 如果 `__boot_info_ptr` 在这个范围内，它会被清零

### 调试输出验证

```
保存前: %rdi = 0x7000
保存后立即读取: __boot_info_ptr = 0x7000 ✅
BSS 清除后读取: __boot_info_ptr = 0x0000 ❌
```

---

## 六、完整的修复

### 修改 boot.S

**保存位置调整**：确保在 BSS 清除之前保存

```asm
_start:
    cli

    /* Output '1' - reached */
    movb $0x31, %al
    outb %al, $0xE9

    /* Setup stack */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Output '2' - stack ready */
    movb $0x32, %al
    outb %al, $0xE9

    /* Save BootInfo pointer BEFORE clearing BSS */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb

    /* Output '3' - BSS cleared */
    movb $0x33, %al
    outb %al, $0xE9
```

**将 __boot_info_ptr 移到 .data 段**：

```asm
/* ==============================================================
 * BootInfo Pointer Storage (.data section - NOT .bss!)
 * ============================================================== */
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0

/* ==============================================================
 * Stack Section (.bss)
 * ============================================================== */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
```

---

## 七、验证

### 修复后的输出

```
OPLJ123G4===CPPGC1V123B===END
```

- `R7000` → `%rdi = 0x7000` ✅
- `D[0000000000007000]` → `__boot_info_ptr = 0x7000` ✅
- `===CPPGC1V123B===END` → C++ 测试全部通过 ✅

### 符号表检查

```bash
$ objdump -t ./build/kernel/mini/mini_kernel | grep boot_info

ffffffff800226e8 g     O .data 0000000000000008 __boot_info_ptr
```

注意现在是 `.data` 段，不是 `.bss`！

---

## 八、经验教训

### 1. 寄存器参数的生命周期

```
System V AMD64 ABI 参数传递:
- 第 1 个参数: %rdi
- 第 2 个参数: %rsi
- 第 3 个参数: %rdx
- ...

这些寄存器是"易失的"，函数调用后可能被修改！
```

### 2. BSS 清除的副作用

```asm
rep stosb  /* 清除 BSS */
```

这个指令：
1. 使用 `%rdi` 作为目标地址 → **破坏参数寄存器**
2. 清除整个 `.bss` 段 → **清除所有 .bss 变量**

### 3. 数据段的选择

| 段 | 特点 | 适用场景 |
|---|---|---|
| `.data` | 有初始值，不会被运行时清除 | 需要持久化的全局变量 |
| `.bss` | 无初始值，运行时清零 | 零初始化的全局变量 |
| `.rodata` | 只读数据 | 常量、字符串 |

**关键原则**：需要在 BSS 清除前/后保留的变量，应该放在 `.data` 段！

### 4. 调试技巧

当遇到参数值异常时：
1. ✅ 在入口处立即打印/保存参数
2. ✅ 单步执行，追踪参数的变化
3. ✅ 检查哪些代码修改了相关寄存器/内存

---

## 九、相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/mini/arch/x86_64/boot.S` | 调整 `__boot_info_ptr` 保存位置，移到 .data 段 |
| `kernel/mini/linker.ld` | 确保 .data 段在 .bss 之前 |

---

## 🎯 总结

> **在内核启动代码中，BSS 清除是一个"危险操作"，会破坏寄存器参数和 .bss 段变量。**
>
> **解决方案：**
> 1. **在清除前保存**寄存器参数到安全位置
> 2. **将持久化变量放在 .data 段**，而不是 .bss 段

在这个案例中：
```
问题: %rdi 在清除 BSS 时被覆盖
解决: 在清除前保存 %rdi 到 __boot_info_ptr（.data 段）
```
