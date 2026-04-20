# 019 · 线程退出崩溃排查

## 现象

生产内核两个线程交替打印 5 轮后崩溃：

```
[A] thread_a done
emulation failure
RIP=00000000deadc0de
```

thread_b 未完成，kernel 直接 triple fault。

## 根因

两个 bug 叠加：

### Bug 1：线程函数无返回地址

`TaskBuilder::build()` 初始化 CpuContext 时：

```cpp
// 修改前
task->ctx.rsp = stack_virt + stack_size;  // 栈顶，无任何内容
task->ctx.rip = entry_point;
```

context_switch 通过 `jmp *56(%rsi)` 跳到入口，不是 `call`。线程函数
return 时 `ret` 弹栈顶内容作为返回地址——栈顶是空的，一路弹到栈底
magic `0xDEADC0DE`。

### Bug 2：exit_current 指针覆盖

即使修复 Bug 1（在栈上放 exit_current 地址），exit_current 本身有 bug：

```cpp
// 修改前
current_ = next;                          // current_ 被覆盖
context_switch(&current_->ctx, &next->ctx);  // from == to，切换到自身
```

`from` 和 `to` 指向同一个 task，context_switch 变成 no-op，执行继续
在已退出的线程栈上，最终还是会崩。

## 修复

### 1. 栈上压入 exit_current 作为返回地址

```cpp
// process.cpp
task->ctx.rsp = stack_virt + stack_size - 8;
*reinterpret_cast<uint64_t*>(task->ctx.rsp) =
    reinterpret_cast<uint64_t>(&Scheduler::exit_current);
task->ctx.rip = reinterpret_cast<uint64_t>(entry_);
```

线程函数 return → ret 弹出 exit_current 地址 → 进入退出流程。

### 2. exit_current 保存 prev 指针

```cpp
void Scheduler::exit_current() {
    Task* prev = current_;          // 先保存
    prev->state = TaskState::Dead;
    prev->sched_class->dequeue(prev);

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        while (1) __asm__ volatile("cli; hlt");
    }
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);  // from ≠ to
}
```

## 修复后输出

```
[A] thread_a iteration 0
[B] thread_b iteration 0
...
[A] thread_a done
[SCHED] Task tid=1 'thread_a' exited
[B] thread_b done
[SCHED] Task tid=2 'thread_b' exited
[SCHED] No more tasks, halting.
```

两个线程完整执行后干净退出，无崩溃。
