# M4: 多核调度

> Per-CPU run queue + 简单 work stealing 负载均衡。
> 每个核心独立调度，空闲时从其他核心偷任务。

## 依赖

- M3 AP 启动 + Per-CPU 数据

## 任务清单

### T1: Per-CPU Run Queue

**文件**: `kernel/proc/scheduler.hpp`（扩展）

每个 CPU 有独立的 run queue：

```cpp
class PerCPUScheduler {
public:
    void enqueue(Task* task);
    void dequeue(Task* task);
    Task* pick_next();

    // Work stealing
    Task* steal_one();  // 被其他 CPU 调用，偷一个任务

    size_t queue_size() const;

private:
    RoundRobin rr_;      // 复用现有 RoundRobin
    Spinlock   lock_;    // 保护本队列
};
```

- [ ] PerCPUScheduler 包装 RoundRobin + lock
- [ ] 每个 CPU 的 PerCpu 中持有 PerCPUScheduler
- [ ] enqueue/dequeue 加锁操作

### T2: 全局调度器改造

**文件**: `kernel/proc/scheduler.hpp`

```cpp
class Scheduler {
public:
    // 现有接口保持兼容
    static void yield();
    static void schedule();
    static void block(Task* task, const char* reason);
    static void unblock(Task* task);

    // 新增
    static Task* current();              // 通过 percpu()->current_task
    static void enqueue(Task* task);     // 放到当前 CPU 的队列
    static void tick();                  // APIC timer tick

private:
    static Task* pick_next();            // 从当前 CPU 队列选
    static Task* try_steal();            // 从其他 CPU 偷
};
```

- [ ] current() 改为从 PerCpu 读取（替代全局变量）
- [ ] yield/schedule 在当前 CPU 上执行
- [ ] enqueue 优先放到当前 CPU，除非指定了 CPU affinity

### T3: Work Stealing

```cpp
Task* Scheduler::try_steal() {
    uint32_t my_cpu = cpu_id();
    for (uint32_t i = 0; i < cpu_count(); i++) {
        if (i == my_cpu) continue;
        auto& other = percpu_for(i)->sched;
        if (other.queue_size() > 1) {
            Task* stolen = other.steal_one();
            if (stolen) return stolen;
        }
    }
    return nullptr;  // 没偷到，进入 idle
}
```

- [ ] pick_next() 失败时调用 try_steal()
- [ ] steal_one() 从队列尾部取（减少竞争）
- [ ] 避免颠簸：只从 queue_size > 1 的 CPU 偷

### T4: CPU Affinity

- [ ] Task 增加 `uint64_t cpu_affinity` 位掩码
- [ ] enqueue 时检查 affinity 决定放入哪个 CPU
- [ ] 默认 affinity = all CPUs
- [ ] 未来支持 sched_setaffinity() syscall

### T5: Idle Thread

每个 CPU 需要一个 idle thread（没有可运行任务时执行）：

```cpp
void idle_thread_func() {
    while (true) {
        // 尝试偷任务
        if (auto* task = try_steal()) {
            enqueue(task);
            continue;
        }
        // 没任务 → halt 等待中断
        __asm__ volatile("hlt");
    }
}
```

- [ ] 每个 AP 启动时创建 idle thread
- [ ] idle thread 优先级最低
- [ ]hlt 等待 APIC timer 或 IPI 唤醒

### T6: 调度 IPI

- [ ] unblock() 时如果目标 CPU 在 idle → 发送 reschedule IPI
- [ ] reschedule IPI handler：标记 need_resched
- [ ] IPI vector 定义（如 0xF0-0xFF 范围）

### T7: 单元测试

- [ ] 双核各 enqueue 不同 task，各自调度正确
- [ ] work stealing：一个 CPU 空闲时偷另一个的任务
- [ ] CPU affinity 限制任务只在指定核心运行
- [ ] QEMU `-smp 4` 四核调度验证

## 产出物

- [ ] `kernel/proc/scheduler.hpp` — Per-CPU run queue + work stealing
- [ ] idle thread 机制
- [ ] 调度 IPI
- [ ] CPU affinity 支持
- [ ] QEMU 多核调度验证
