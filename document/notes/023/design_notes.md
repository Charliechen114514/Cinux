# 023_syscall 设计笔记

## 测试输出

```
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000
[USER] Hello from Ring 3!
[SYSCALL] sys_exit: no scheduler, halting.
```

## 为什么 sys_exit 会 halt 而不是 yield

`sys_exit` 实现中有两个分支：

1. **调度器已初始化**：标记 task 为 Dead，调用 `Scheduler::yield()` 让出 CPU
2. **调度器未初始化**：打印提示并 `cli; hlt` 死循环

当前 kernel_main 在 `launch_first_user()` 之前没有启动调度器（milestone 020 实现了调度器，但此测试路径未启用），因此走的是分支 2。这是预期行为。

## 为什么这样设计是合理的

对于 milestone 023 的目标（验证 syscall 机制），单任务测试足够：

- Ring 3 切换成功 ✓
- `syscall` 指令正确触发内核入口 ✓
- SWAPGS + 内核栈切换正确 ✓
- trap frame 保存/恢复正确 ✓
- dispatch 到 `sys_write` 并输出到串口 ✓
- `sys_exit` 干净地终止（halt）✓

调度器集成将在后续 milestone（024_shell）中完成，届时需要在 `launch_first_user()` 之前启动调度器，使 `sys_exit` 走 yield 路径，shell 才能作为常驻进程运行。

## sys_exit 的防御性设计

无论调度器是否运行，`sys_exit` 都能正确处理：

```cpp
if (Scheduler::is_initialized()) {
    Scheduler::yield();
} else {
    kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
    while (1) { asm volatile("cli; hlt"); }
}
```

这种设计使得 syscall 模块可以在有/无调度器的环境下都能工作，降低了里程碑间的耦合。
