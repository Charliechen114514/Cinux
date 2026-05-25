---
title: #GP 栈对齐
---

# 030 #GP 异常排查：ISR 栈对齐违反 System V ABI

## 现象

`make run` 启动后，在打印 `[GUI] ===== Milestone 030: GUI Window Manager =====` 后立即触发 #GP：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81001DBB   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  RSP   = 0xFFFF800008047EF8   SS  = 0x0018
  RAX=0x0000000000000041  RBX=0x0000000000000000
  RCX=0x00000000FFFFFF01  RDX=0x0000000000000003
  RSI=0x0000000000000001  RDI=0x0000000000000000
  ...
```

## 触发路径

```
gui_start()
  → Mouse::init()            // 操作 PS/2 控制器 (0xA8, 0x20, 0x60, 0xD4, 0xF4)
    → 触发 IRQ1 (键盘)       // PS/2 控制器操作产生虚假键盘中断
      → irq1_stub
        → Keyboard::irq1_handler()
          → 编译器生成 movaps %xmm0, (%rsp)  // ← #GP
```

`Mouse::init()` 通过 8042 PS/2 控制器命令启用 AUX 设备（鼠标）时，控制器状态变化会触发一个虚假的 IRQ1（键盘中断）。这是 PS/2 硬件的已知行为。

## 根因分析

### x86_64 System V ABI 栈对齐规则

调用 `call` 指令前，RSP 必须 16 字节对齐。`call` 压入 8 字节返回地址后，被调函数入口处 RSP ≡ 8 (mod 16)。编译器依赖此约定生成 `movaps` 等 SSE 指令——这些指令要求操作数地址 16 字节对齐。

### ISR stub 原来的栈布局（修复前）

```
CPU 自动压入 (IRQ):  SS, RSP, RFLAGS, CS, RIP  = 5 × 8 = 40 bytes
ISR stub 压入:       error_code(0), rax..r15    = 16 × 8 = 128 bytes
                                                        总计 = 168 bytes
call handler 压入:   return address                = 8 bytes
                                                        总计 = 176 bytes
```

176 是 16 的倍数，意味着 `call` 之后 handler 入口处 **RSP ≡ 0 (mod 16)**，违反了 System V ABI 要求的 RSP ≡ 8 (mod 16)。

handler 内部 `push %rbx; sub $0x20, %rsp` 后 RSP ≡ 8 (mod 16)，此时 `movaps %xmm0, (%rsp)` 因地址未 16 字节对齐而触发 #GP。

### 正确的栈布局（修复后）

在压完 GPR 后额外压入 8 字节对齐 padding：

```
CPU 自动压入 (IRQ):  SS, RSP, RFLAGS, CS, RIP  = 5 × 8 = 40 bytes
ISR stub 压入:       error_code(0), rax..r15    = 16 × 8 = 128 bytes
ISR stub 压入:       alignment padding            = 8 bytes
                                                        总计 = 176 bytes
call handler 压入:   return address                = 8 bytes
                                                        总计 = 184 bytes
```

184 ≡ 8 (mod 16)，handler 入口处栈对齐正确。

## 修复方案

修改 `kernel/arch/x86_64/interrupts.S` 中的 `ISR_NOERRCODE` 和 `ISR_ERRCODE` 两个宏：

```asm
    pushq %r15                        # 最后一个 GPR

    # --- 新增：栈对齐 padding ---
    pushq $0                          # alignment padding (8 bytes)

    # 调整 InterruptFrame* 指针跳过 padding
    leaq 8(%rsp), %rdi                # 指向 padding 之后的 r15（而非 padding 本身）
    call \handler
    addq $8, %rsp                     # 弹出 alignment padding

    popq %r15                         # 恢复 GPR（顺序不变）
```

关键点：
- `leaq 8(%rsp), %rdi` 确保传递给 C handler 的 `InterruptFrame*` 指针仍然指向 `r15` 字段，`InterruptFrame` 结构体布局无需修改
- handler 返回后 `addq $8, %rsp` 弹出 padding，再正常恢复 GPR

## 为什么之前没触发

这个 bug 一直存在，但之前的 IRQ handler 恰好没有生成 `movaps` 指令。直到 `Keyboard::irq1_handler` 中引入了 GUI 双路分发代码（`cinux::drivers::Mouse::event_queue().enqueue(gui_ev)`），编译器在函数内使用了 XMM 寄存器进行优化，生成了 `movaps`，才暴露了栈对齐问题。

## 经验教训

1. **ISR stub 必须保证栈 16 字节对齐**——这是 x86_64 System V ABI 的硬性要求，不是可选的
2. **硬件副作用可能触发意外中断**——PS/2 控制器操作可能产生虚假 IRQ，ISR 必须随时能安全执行
3. **栈对齐 bug 是静默的**——在简单 handler 中不会触发，只有编译器恰好生成 SSE 指令时才会暴露，排查难度较高
