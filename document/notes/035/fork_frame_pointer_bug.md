---
title: Fork 帧指针 Bug
---

# 035 fork() 帧指针依赖排查报告

## 现象

点击 Shell 图标 fork 子进程后，系统触发 **Double Fault (#DF)** 并彻底卡死。

```
==== EXCEPTION: #DF (vector 8) ====
  RIP   = 0xFFFFFFFF8100E0DE   CS  = 0x0010
  RSP   = 0x00007FFFF947F008   SS  = 0x0000
  R11   = 0x00000000FD000000
[FATAL] Double Fault -- halting.
```

关键线索：**RSP = 0x00007FFFF947F008 是用户空间地址**，但此时 CPU 处于内核态 (CS=0x0010)。内核在用用户空间栈运行，一旦发生异常，异常处理程序试图 push 到该用户栈 → 再次缺页 → Double Fault。

## 根因

### fork() 的 RBP 假设

fork() 通过内联汇编读取 RBP，假设它是当前函数的帧指针（frame pointer）：

```cpp
// process.cpp — fork() 内部
uint64_t current_rsp;
uint64_t current_rbp;
__asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
__asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

// 用 RBP 定位 fork() 的返回地址：假设 [RBP+8] = 返回地址
child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;
child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);  // 假设 [RBP] = 调用者的 RBP
```

### Release 模式下的真相

项目使用 `-DCMAKE_BUILD_TYPE=Release`，编译器开启 `-O2`，**默认包含 `-fomit-frame-pointer`**。

这意味着：
- RBP **不再是帧指针**，编译器可以将其当作通用寄存器使用
- `[RBP+8]` 不再是 fork() 的返回地址，而是内存中某个随机位置
- `ctx.rsp` 计算结果为垃圾值，子进程的栈指针指向了用户空间
- `ctx.rbp` 同样是垃圾值

### 崩溃链条

```
fork() 中 RBP 不是帧指针
  → ctx.rsp = 垃圾值（用户空间地址）
  → ctx.rbp = 垃圾值
  → 调度器切换到子进程，恢复 ctx.rsp / ctx.rbp
  → 子进程在用户空间地址上"运行"
  → 任何异常/中断 → push 到用户空间 RSP
  → 用户空间地址在子进程的新地址空间中未映射
  → 页错误 → 再页错误 → Double Fault → 死机
```

## 修复

在 fork() 上强制启用帧指针：

```cpp
__attribute__((optimize("no-omit-frame-pointer"), noinline))
int fork(PidAllocator& pid_alloc) {
    ...
}
```

- `optimize("no-omit-frame-pointer")` — 该函数保留帧指针，RBP 回归传统角色
- `noinline` — 防止内联（内联后函数边界消失，帧指针语义也会变化）

## 调试方法回顾

通过在 3 个关键位置添加节流日志定位问题：

| 位置 | 日志内容 | 发现 |
|------|---------|------|
| `gui_tick_callback` | create_shell_terminal 进入/返回 | 确认 parent 正常返回 |
| `handle_pf` demand-paging | 任务名 + 计数器 | 定位到 0xFD000000 洪水（huge page bug） |
| `schedule()` | prev 任务名 + 状态 | 确认调度器行为 |

第一个 bug（huge page）修复后，demand-paging 消失，但 Double Fault 浮出水面——异常帧中的 RSP 清楚地指向了用户空间，直接暴露了帧指针问题。

## 经验教训

1. **永远不要在 Release 模式下假设 RBP 是帧指针。** 如果必须通过寄存器定位栈帧，要么用 `__attribute__((optimize("no-omit-frame-pointer")))` 保护该函数，要么改用 `__builtin_return_address(0)` / `__builtin_frame_address(0)` 等编译器内置函数。

2. **内联汇编读取的寄存器值在优化模式下含义会变。** `movq %%rbp, %0` 读到的不一定是帧指针；`movq %%rsp, %0` 仍然可靠（RSP 始终是栈指针）。

3. **Double Fault 中 RSP 在用户空间 = 子进程栈设置错误。** 这是 fork 类实现的经典症状。

4. **分步排查，一次只修一个问题。** 本例中 huge page bug 和帧指针 bug 同时存在，先修前者才能观察到后者的完整表现。
