---
title: IF 标志丢失
---

# 020 · context_switch 丢失 IF 标志导致后续线程中断全关

## 现象

6 个线程 × 10 次迭代 × 20M 忙循环。第一次抢占成功（A 跑了 2 次后切到 B），但之后 B/C/D/E/F 全部顺序跑完，再也没有被抢占：

```
[A] tid=2 iter 1/10
[A] tid=2 iter 2/10          ← 第一次抢占发生在这里
[B] tid=3 iter 1/10
...
[B] tid=3 iter 10/10         ← B 跑完，从未被抢占
[B] done
[C] tid=4 iter 1/10
...
[F] done
[A] tid=2 iter 3/10          ← A 回来继续
...
[A] done
```

## 根因

`context_switch.S` 只保存/恢复 callee-saved 寄存器（r15-r12, rbp, rbx, rsp, rip），**不涉及 RFLAGS**。

抢占发生时的调用链：

```
IRQ0 → ISR stub (CPU 清除 IF) → pit_irq0_handler → Scheduler::tick() → schedule() → context_switch
```

进入 ISR 时，CPU 自动将 IF 清零（interrupt gate 语义）。`context_switch` 切换到新任务时，新任务**继承了当前 IF=0**。

- **新任务（首次运行）**：`ctx.rip` 指向入口函数，`jmp *56(%rsi)` 直接跳转，IF 仍为 0 → 永远收不到定时器中断
- **被抢占过的任务（恢复运行）**：`ctx.rip` 指向 `.restore`，恢复后通过 `ret` 链回到 ISR stub → `IRETQ` 从中断帧还原原始 RFLAGS（IF=1） → 中断恢复

所以只有第一个被抢占的任务（A）能恢复中断，后续所有新启动的线程（B-F）都带着 IF=0 运行。

## 修复

在 `context_switch.S` 中，切换栈之后、跳转到新任务之前加一条 `sti`：

```asm
    movq 48(%rsi), %rsp          # 切换到新任务栈
    sti                           # 开中断
    jmp *56(%rsi)                 # 跳转到新任务
```

为什么安全：
- **新任务**：以 IF=1 启动，定时器中断正常到达 → 抢占正常
- **被抢占的任务**：恢复到 `.restore` → `ret` 链回到 ISR → `IRETQ` 还原原始 RFLAGS，`sti` 是无害冗余
- **嵌套中断风险**：从 `.restore` 到 `IRETQ` 的退栈路径极短（微秒级），100Hz 定时器（10ms 间隔）在此窗口命中的概率可忽略

## 教训

协作式 context_switch 设计时不考虑 RFLAGS 是合理的——总是在明确的调用点切换，IF 不变。但一旦从中断上下文调用同一个 `context_switch`，就必须保证新任务的中断状态正确。这是从 cooperative 迈向 preemptive 的经典陷阱。

一种更精细的方案是把 RFLAGS 加入 `CpuContext` 并在保存/恢复时使用 `pushfq/popfq`，但对当前仅内核线程的场景来说，在切换点 `sti` 是最简洁的修复。
