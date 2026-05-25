---
title: 时间片过长
---

# 020 · 时间片过长导致抢占从未触发

## 现象

3 个线程各跑 5 次迭代，完全顺序执行，没有任何交错：

```
[A] thread_a iteration 0
...
[A] thread_a iteration 4
[B] thread_b iteration 0
...
[B] thread_b iteration 4
[C] thread_c iteration 0
...
```

## 分析

PIT 配置为 100Hz（每 tick 10ms），`DEFAULT_TIME_SLICE = 10`，即 **100ms 才触发一次抢占**。

忙循环 `for (volatile int j = 0; j < 1000000; j++) {}` 在 QEMU TCG 模式下执行极快，单次迭代 < 5ms。每个线程 5 次迭代总耗时 < 50ms，远小于 100ms 时间片——线程在自己的时间片内就跑完了。

## 修复

1. **缩短时间片**：`DEFAULT_TIME_SLICE` 从 10 改为 2（20ms）。
2. **增大忙循环**：迭代次数从 1M 提升到 20M，确保每个线程跨越多个时间片。

## 教训

在虚拟化环境中，简单的 CPU 密集循环比预期快得多。测试抢占调度时，要确保工作负载足够大，或者时间片足够小，让定时器中断有机会介入。
