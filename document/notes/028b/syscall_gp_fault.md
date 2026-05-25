---
title: Syscall GP Fault
---

# syscall GP Fault 排查报告

## 现象

在 QEMU 中执行 `touch /hello2.txt` 时触发 General Protection Fault：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  ERROR CODE = 0x0000000000000000
========================================
```

## 定位过程

### 1. 确定崩溃函数

通过 `nm` 反查崩溃地址：

```
ffffffff81005d20 T Ext2::create(uint32_t, char const*, uint32_t)
ffffffff81005e90 T Ext2DirOps::create(Inode*, char const*, uint32_t)
```

RIP=0x5D97 落在 `Ext2::create()` 内部。

### 2. 反汇编定位崩溃指令

```
ffffffff81005d7d:  lea    0x80(%rsp),%rdx    ; new_disk 栈变量地址
ffffffff81005d8e:  mov    %rdx,%rax
ffffffff81005d97:  movaps %xmm0,(%rax)       ; ← GP fault 在这里
```

`movaps` 要求 16 字节对齐。RAX = RSP + 0x80 = 0x...D78，0x78 % 16 = 8，未对齐。

### 3. 根因分析

崩溃根因是 **syscall 入口未保证 16 字节栈对齐**。

调用链：`syscall_entry (asm)` → `syscall_dispatch (C)` → ... → `Ext2::create (C)`

`syscall_entry` 中 push 了 12 个寄存器构建 trap frame（96 字节），再 push 第 7 个 C 参数（8 字节），共 13 次 push = 104 字节。104 % 16 = 8，导致 `call syscall_dispatch` 前 RSP 不是 16 的倍数。

SysV AMD64 ABI 要求：`call` 指令前 RSP 必须是 16 字节对齐的。由于差了 8 字节，后续所有函数的栈布局都偏移了 8 字节。当编译器生成 SSE `movaps`（要求 16 字节对齐）来零初始化栈上的 `Ext2Inode` 结构体时，触发 GP fault。

### 4. 为什么之前的 syscall 没有崩溃

此前已注册的 syscall（`sys_read`、`sys_write`、`sys_open` 等）在执行路径上恰好没有触发 SSE 对齐指令。`sys_creat` 是第一个深入调用 ext2 复杂逻辑的 syscall，其中 `Ext2::create()` 用循环清零 `Ext2Inode`（编译器优化为 `movaps`），才首次暴露了这个问题。

## 修复

在 `kernel/arch/x86_64/syscall.S` 中，push 第 7 个参数前加 `subq $8, %rsp` 对齐栈：

```asm
    movq 72(%rsp), %rax                # load a6
    subq $8, %rsp                      # ← 新增：16 字节对齐
    pushq %rax                         # push 7th arg

    movq 40(%rsp), %rdi                # offsets +8 (now +16 due to padding)
    ...
    call syscall_dispatch
    addq $16, %rsp                     # remove arg + padding (was $8)
```

修复后所有偏移量从 +8 调整为 +16（多了一层 padding），清理时 `addq` 从 $8 改为 $16。

## 教训

1. **syscall/中断入口的栈对齐是硬性要求**：x86_64 SysV ABI 假定 `call` 前 RSP 是 16 的倍数。任何手写汇编入口（syscall、interrupt handler）都必须维护这个不变量，否则所有使用 SSE 指令的编译代码都可能随机崩溃。

2. **对齐 bug 有隐蔽性**：bug 不在崩溃函数中，而在调用链最顶层的汇编入口中。简单的 syscall（浅调用栈）不会触发，只有深入调用且碰上 SSE 指令时才暴露。

3. **排查方法**：GP fault + error_code=0 → 先看是否为对齐问题；反汇编确认 `movaps`/`movdqa` 等对齐指令；回溯栈布局计算 RSP 对齐状态。
