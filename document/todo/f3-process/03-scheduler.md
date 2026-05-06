# M4: 调度器接口验证与增强

> 现有 SchedulingClass 已支持插拔式调度算法。
> 验证接口完备性，小幅增强优先级支持。

## 目标

验证当前 SchedulingClass 接口能否快速集成新调度算法（如优先级调度、CFS）。
如果接口不完整则补齐，但不引入新调度器实现。

## 现有代码

- `kernel/proc/scheduler.hpp` — SchedulingClass 基类 + RoundRobin 实现
- `kernel/proc/process.hpp` — Task 有 priority 字段（未被使用）

## 任务清单

### T1: SchedulingClass 接口审查

**文件**: `kernel/proc/scheduler.hpp`

当前接口：
```cpp
class SchedulingClass {
    virtual void enqueue(Task* task) = 0;
    virtual void dequeue(Task* task) = 0;
    virtual Task* pick_next() = 0;
    virtual const char* name() const = 0;
};
```

审查是否需要补充：
- [ ] `task_tick(Task* current)` — 时间片耗尽回调（RoundRobin 在外部处理，应该内聚）
- [ ] `task_fork(Task* parent, Task* child)` — fork 时子进程调度参数继承
- [ ] `task_deadline(Task* task)` — 用于实时调度查询
- [ ] 按需添加缺失接口

### T2: 优先级字段启用

**文件**: `kernel/proc/scheduler.hpp`

- [ ] RoundRobin::enqueue 按 priority 排序插入（简单实现）
- [ ] 高优先级任务优先调度
- [ ] 同优先级 Round Robin
- [ ] priority 值越小越优先（Linux 风格）

### T3: 调度器切换机制

**文件**: `kernel/proc/scheduler.hpp`

```cpp
class Scheduler {
public:
    // 注册调度类
    void register_class(int priority, SchedulingClass* cls);

    // pick_next 按优先级遍历调度类
    Task* pick_next();
};
```

- [ ] Scheduler 支持注册多个 SchedulingClass
- [ ] pick_next() 按调度类优先级遍历
- [ ] 验证：新增一个 PriorityRR 类只需继承 SchedulingClass + register_class

### T4: 验证文档

- [ ] 写一段伪代码示例：如何添加一个新的调度算法
- [ ] 作为代码注释写在 scheduler.hpp 头部

## 产出物

- [ ] `kernel/proc/scheduler.hpp` — 增强接口
- [ ] 优先级感知的 RoundRobin
- [ ] 多调度类注册机制
- [ ] 不改变现有行为（向后兼容）
