# Freestanding 环境下的两个经典陷阱：从神秘的 0x2a00000000 说起

> 看着 debugcon 输出一串奇怪的数字：`OPLJ12R70003G4D[0000002A00000000]`

这是一个周五的深夜，我的内核开发项目又遇到了奇怪的问题。

- `R7000` 看起来很正常 —— 那是 bootloader 传递给我的 BootInfo 指针地址
- 但 `D[0000002A00000000]` 完全不对 —— 42 左移 32 位？这是什么鬼？

接下来的几个小时里，我从一个简单的指针传递问题开始，一路挖到了链接器的底层行为，发现了两个 freestanding 环境下的经典陷阱。

---

## 第一部分：BSS 清除破坏参数寄存器

### 初步诊断

一切从 `mini_kernel_main` 函数开始：

```cpp
// kernel/mini/main.cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)boot_info_addr;

    // 调试显示 boot_info = 0xffffffff80022188 ❌
    // 预期应该是 boot_info = 0x7000
}
```

参数值完全错了。我首先确认 bootloader 传参是否正确，在 `boot.S` 的 `_start` 入口处添加了调试输出：

```asm
/* Debug: Output %rdi lower 16 bits */
movl %edi, %eax
shrl $12, %eax    /* Get bits 15-12 */
andl $0xF, %eax
addb $'0', %al
outb %al, $0xE9
... (重复输出 4 个十六进制数字)
```

输出是 `R7000`，说明 `%rdi = 0x7000` —— **bootloader 正确传递了参数**。

问题一定在内核内部。

### 追踪破坏点

我的 boot.S 大致是这样的：

```asm
_start:
    cli

    /* Setup stack */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Save BootInfo pointer BEFORE clearing BSS */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi   /* <-- 这里！ */
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb                 /* 清除 BSS */

    call _init_global_ctors

    /* Call C++ main */
    movq __boot_info_ptr, %rdi
    call mini_kernel_main
```

看到那行注释了吗？`movq $__bss_start, %rdi` —— 这行代码把 `%rdi` 覆盖了！

### 反直觉的结论

这是一个非常反直觉的问题：

> **System V AMD64 ABI 的参数寄存器在函数开始第一行代码执行前就已经被破坏了。**

根据 System V AMD64 ABI，第一个参数通过 `%rdi` 传递。但我们的启动代码需要清除 BSS 段，而 `rep stosb` 指令恰好使用 `%rdi` 作为目标地址寄存器。这个"巧合"导致了参数被覆盖。

### 第一层修复

最简单的解决方案是使用一个安全寄存器保存参数：

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

这解决了寄存器破坏的问题，但那个神秘的 `0x2a00000000` 依然存在。

---

## 第二部分：.bss 符号地址冲突

### 神秘值的线索

`0x2a00000000 = 42 << 32` —— 这个 42 从哪来？

我在代码中搜索，找到了这个：

```cpp
static int global_construction_count = 0;

class GlobalCounter {
public:
    GlobalCounter() {
        global_construction_count = 42;  // 0x2A = 42
        debugcon_putc('G');
    }
};

GlobalCounter global_counter;
```

这个值居然与全局构造函数相关！但我还是想不通，为什么 `global_construction_count` 的值会跑到 `__boot_info_ptr` 里去？

### 符号表检查

我决定用 `objdump` 查看符号表：

```bash
$ objdump -t ./build/kernel/mini/mini_kernel | grep -E "(boot_info|global_construction|global_counter)"
```

输出让我大吃一惊：

```
ffffffff800226ec l     O .bss  0000000000000004 _ZL25global_construction_count
ffffffff8002042c l     F .text 0000000000000012 _GLOBAL__sub_I_global_counter
ffffffff800226f0 g       .bss  0000000000000000 __bss_end
ffffffff800226e8 g       .bss  0000000000000000 __boot_info_ptr
ffffffff800226e8 g     O .bss  0000000000000001 global_counter
ffffffff800206e0 g       .bss  0000000000000000 __bss_start
```

注意这两行：
```
ffffffff800226e8  global_counter        (1 byte)
ffffffff800226e8  __boot_info_ptr       (8 bytes)
                  ^^^^^^^^^^^^^^^^
                  同一个地址！
```

**`global_counter` 和 `__boot_info_ptr` 在同一个地址！**

### 内存布局分析

```
地址                 变名                             大小
────────────────────────────────────────────────────────────
0xffffffff800226e8  global_counter                     1 byte
0xffffffff800226e8  __boot_info_ptr (被覆盖!)         8 bytes
0xffffffff800226ec  global_construction_count         4 bytes
```

执行流程变成了这样：
1. bootloader 传递 `%rdi = 0x7000`
2. `movq %rdi, __boot_info_ptr` → 向地址 `0x800226e8` 写入 `0x7000`
3. `rep stosb` (BSS 清除) → 清除 `0x800226e8` 开始的区域
4. `_init_global_ctors` 调用
5. `global_counter.GlobalCounter()` 执行
6. 由于某种原因，`0x800226e8` 被写入了 `0x2a00000000`

### 更反直觉的结论

这里有一个更反直觉的事实：

> **.bss 段的变量不应该放在 .bss 段里。**

我的 `__boot_info_ptr` 是这样定义的：

```asm
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

问题在于：
1. `.bss` 段会被运行时清零 —— 刚保存的值立即被清除
2. 链接器可能将不同来源（汇编 `.S` 和 C++ `.cpp`）的符号分配到同一地址
3. C++ 全局变量的初始化可能覆盖汇编定义的符号

### 第二层修复

解决方案很简单 —— 将 `__boot_info_ptr` 移到 `.data` 段：

```asm
/* ============================================================== */
/* BootInfo Pointer Storage (.data section - NOT .bss!)         */
/* ============================================================== */
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0              /* 显式初始化为 0 */

/* ============================================================== */
/* Stack Section (.bss)                                          */
/* ============================================================== */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
```

### 验证修复

重新编译后，符号表显示：

```
ffffffff800226e8 g     O .data 0000000000000008 __boot_info_ptr     ← 现在 .data 段！
ffffffff800226f0 l     O .bss  0000000000000004 _ZL25global_construction_count
ffffffff800226f4 g     O .bss  0000000000000001 global_counter       ← 不再冲突！
```

debugcon 输出也正常了：

```
OPLJ12R70003G4D[0000000000007000]
                  ^^^^^^^^^^^^^^^^
                  现在是正确的 0x7000！
```

---

## 技术总结

### 两个 Bug 的本质

| Bug | 表面现象 | 根本原因 | 解决方案 |
|-----|---------|---------|---------|
| BSS 清除破坏参数 | 参数值错误 | `rep stosb` 使用 %rdi 作为目标地址 | 使用安全寄存器保存参数 |
| 符号地址冲突 | 神秘的 0x2a00000000 | 汇编/C++ 符号在同一地址 | 关键变量放 .data 段 |

### 段的选择原则

| 段 | 特点 | 使用场景 |
|---|---|---|
| `.data` | 有初始值，不会被运行时清除 | 需要持久化的全局变量、关键数据 |
| `.bss` | 无初始值，运行时清零 | 普通零初始化全局变量 |
| `.rodata` | 只读 | 常量、字符串 |

**核心原则**：需要在 BSS 清除前/后保留的变量，应该放在 `.data` 段。

### Freestanding 环境的特殊性

在 freestanding 环境下：
- 没有 libc 运行时保护
- 需要手动管理 BSS 清除
- 链接器行为与 hosted 环境不同
- ABI 规范不能保证所有情况下的安全

---

## 调试技巧

### 符号表检查

```bash
# 查看所有符号及其地址
objdump -t kernel.elf | sort

# 查看特定段的符号
objdump -t kernel.elf | grep "\.bss"

# 检查符号冲突
objdump -t kernel.elf | awk '{print $1, $NF}' | sort | uniq -d
```

### Debugcon 调试

- 在关键位置输出字符标记（如 '1', '2', '3'）
- 实现十六进制转储函数
- 实时追踪执行流程

---

## 结尾

> **这两个 bug 是 freestanding 环境的经典陷阱：你以为是代码问题，其实是链接器问题；你以为是内存问题，其实是段选择问题。**

从神秘数字 `0x2a00000000` 到完美的 `0x7000`，这个过程让我深刻理解了系统底层的运行机制。问题的解决往往不在于复杂的代码技巧，而在于对基础概念的清晰理解 —— ABI 规范、链接器行为、内存布局。

如果你也在开发操作系统或 freestanding 程序，希望这些经验能帮你少走弯路。当你遇到奇怪的值时，记得检查：
1. 参数寄存器是否被破坏
2. 符号地址是否冲突
3. 段的选择是否正确

下一个陷阱可能就在前面等着你，但现在你有了更好的工具来应对。

---

*注：本文基于 Cinux OS 开发过程中的真实问题记录。完整代码和笔记可在 [GitHub](https://github.com/your-repo) 查看。*
