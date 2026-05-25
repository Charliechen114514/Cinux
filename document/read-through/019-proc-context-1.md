---
title: 019-proc-context-1 · 进程上下文
---

# 019-1 通读：Task 数据结构与 TaskBuilder 设计——从零开始构建线程控制块

## 概览

本文是 tag `019_proc_context` 三篇通读教程的第一篇，聚焦 `process.hpp` 中的 `TaskState` 枚举、`CpuContext` 结构体、`Task` TCB 以及 `TaskBuilder` 流式构建器。这些类型是调度框架的底层积木——`CpuContext` 描述上下文切换时的寄存器快照，`Task` 把内核线程的全部状态打包在一起，`TaskBuilder` 提供安全的构造接口。我们从 `TaskState` 开始，然后深入 `CpuContext` 偏移校验、完整 `Task` 结构体字段分析，最后看 `TaskBuilder::build()` 的栈分配和上下文初始化实现。

## 架构图

```
    TaskBuilder::build() 创建流程：
    ┌─────────────────────────────────────────────────────────────┐
    │  TaskBuilder                                                 │
    │    .set_entry(func)                                          │
    │    .set_name("thread_a")                                     │
    │    .set_priority(1)                                          │
    │    .build() ──→ Task TCB                                     │
    │                  ├── CpuContext ctx (callee-saved 快照)      │
    │                  │     r15 r14 r13 r12 rbp rbx rsp rip      │
    │                  ├── TaskState state (= Ready)               │
    │                  ├── tid, priority, name                     │
    │                  ├── kernel_stack / kernel_stack_top         │
    │                  ├── addr_space (nullptr = 纯内核线程)       │
    │                  └── sched_class → RoundRobin               │
    │                                                              │
    │  栈布局（4 pages = 16 KB）：                                │
    │    ┌──────────┐  ← kernel_stack_top                         │
    │    │ exit_cur │  ← ctx.rsp 指向这里                         │
    │    │  (free)  │                                              │
    │    │  (free)  │                                              │
    │    │ 0xDEADC0DE│ ← 栈底溢出检测魔数                         │
    │    └──────────┘  ← kernel_stack                              │
    └─────────────────────────────────────────────────────────────┘

    线程生命周期状态机：
    Ready ──→ Running ──→ Ready (yield)
                  │
                  └──→ Blocked (block) ──→ Ready (unblock)
                  └──→ Dead (exit, 被回收)
```

## 代码精讲

### TaskState 枚举——线程的生命周期

```cpp
enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Dead
};
```

`TaskState` 用 `uint8_t` 作为底层类型，一个枚举值只占 1 字节。四个状态对应线程的核心生命周期：`Ready` 表示等待调度，`Running` 表示正在执行，`Blocked` 表示因等待资源而被挂起，`Dead` 是线程退出后的终态。对比 Linux 的 `TASK_RUNNING` / `EXIT_ZOMBIE` 等状态，Cinux 的状态集是一个极简子集，足够支撑合作调度，也为未来的抢占和进程回收预留了扩展点。

### CpuContext——callee-saved 寄存器快照

```cpp
struct alignas(16) CpuContext {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rip;
};

static_assert(offsetof(CpuContext, r15) == 0, "r15 at offset 0");
static_assert(offsetof(CpuContext, r14) == 8, "r14 at offset 8");
static_assert(offsetof(CpuContext, r13) == 16, "r13 at offset 16");
static_assert(offsetof(CpuContext, r12) == 24, "r12 at offset 24");
static_assert(offsetof(CpuContext, rbp) == 32, "rbp at offset 32");
static_assert(offsetof(CpuContext, rbx) == 40, "rbx at offset 40");
static_assert(offsetof(CpuContext, rsp) == 48, "rsp at offset 48");
static_assert(offsetof(CpuContext, rip) == 56, "rip at offset 56");
static_assert(sizeof(CpuContext) == 64, "CpuContext must be 64 bytes");
```

这段代码背后有一个关键设计决策：为什么只保存 8 个 GPR？答案是 System V AMD64 ABI 的调用约定——r15、r14、r13、r12、rbp、rbx 是 callee-saved 寄存器，被调用函数必须保证它们在返回时一致；rax、rcx、rdx 等是 caller-saved，编译器已经在调用 `context_switch()` 之前把它们保存在栈上了。所以只需保存 callee-saved 的 6 个 GPR 加上 RSP 和 RIP。Intel SDM Vol. 3A 第 8.7 节明确指出 64 位模式不支持硬件任务切换，必须由软件完成。整个结构体 `alignas(16)`，`sizeof` 为 64 字节，所有偏移量通过 `static_assert` 编译期锁定。

### Task 结构体——线程控制块

```cpp
struct Task {
    CpuContext ctx;
    TaskState state;
    uint64_t tid;
    uint64_t priority;
    uint64_t kernel_stack;
    uint64_t kernel_stack_top;
    cinux::mm::AddressSpace* addr_space;
    const char* name;
    SchedulingClass* sched_class;
};
```

`Task` 是整个调度子系统的核心数据结构，9 个字段各司其职。`ctx` 保存上下文切换时的寄存器快照，作为 `Task` 的第一个字段，它的地址和 `Task` 的地址完全相同，省去偏移计算。`state` 跟踪线程生命周期——`Ready`（等待调度）、`Running`（正在执行）、`Blocked`（等待资源）、`Dead`（已退出）。对比 Linux 的 `TASK_RUNNING`/`EXIT_ZOMBIE` 等比特位标志集，Cinux 用 scoped enum 更加类型安全。

`tid` 是单调递增的唯一任务标识。`priority` 留给未来的优先级调度。`kernel_stack` 和 `kernel_stack_top` 记录栈的虚拟地址范围——前者用于定位栈底溢出魔数，后者用于设置 TSS 的 RSP0。`addr_space` 指向进程私有页表（纯内核线程为 nullptr），`name` 是人类可读的名字（指向静态字符串，不持有所有权），`sched_class` 指向该任务所属的调度策略对象。对比 xv6 的 `struct proc`（包含 context 指针、kstack、pgdir、state、pid、parent 等），Cinux 的 `Task` 更精简——当前只有调度和内存管理所需的字段，后续的文件描述符、进程树等信息会在对应的 tag 中逐步添加。

### TaskBuilder——流式构建接口

```cpp
class TaskBuilder {
public:
    TaskBuilder() = default;
    TaskBuilder& set_entry(void (*entry)());
    TaskBuilder& set_name(const char* name);
    TaskBuilder& set_priority(uint64_t priority);
    TaskBuilder& set_addr_space(cinux::mm::AddressSpace* space);
    TaskBuilder& set_sched_class(SchedulingClass* sched_class);
    Task* build();

    static constexpr uint64_t STACK_MAGIC = 0xDEADC0DE;
    static constexpr uint64_t STACK_PAGES = 4;

private:
    void (*entry_)()                      = nullptr;
    const char*              name_        = "unnamed";
    uint64_t                 priority_    = 0;
    cinux::mm::AddressSpace* addr_space_  = nullptr;
    SchedulingClass*         sched_class_ = nullptr;
};
```

`TaskBuilder` 用 Builder 模式封装线程构造的复杂性，所有 setter 返回 `*this` 支持链式调用。`build()` 分配 TCB、映射内核栈、初始化 CpuContext、写入溢出检测魔数。`STACK_MAGIC = 0xDEADC0DE` 写在栈底，调试时看到 RIP 跳到这个地址就知道发生了栈溢出。`STACK_PAGES = 4` 意味着每个内核线程的栈是 16 KB。接下来看 `process.cpp` 中 `build()` 的完整实现。

### TaskBuilder::build()——构造线程的全部细节

```cpp
Task* TaskBuilder::build() {
    if (entry_ == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: entry point is null\n");
        return nullptr;
    }

    // Step 1: Allocate the Task struct from the kernel heap
    auto* task = new (std::align_val_t{alignof(Task)}) Task;
    if (task == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: TCB allocation failed\n");
        return nullptr;
    }

    // Step 2: Zero-initialise the task
    for (uint8_t* p = reinterpret_cast<uint8_t*>(task);
         p < reinterpret_cast<uint8_t*>(task + 1); p++) {
        *p = 0;
    }
```

函数一开头就做了防御性检查：如果 `entry_` 为空，直接返回 `nullptr`。然后用 placement new 在内核堆上分配 `Task`，对齐要求是 `alignof(Task)`。分配完后手动逐字节清零，因为内核堆的 `new` 不保证零初始化。

```cpp
    // Step 3: Allocate contiguous physical pages for the kernel stack
    uint64_t stack_phys = cinux::mm::g_pmm.alloc_pages(STACK_PAGES);
    if (stack_phys == 0) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: stack allocation failed\n");
        delete task;
        return nullptr;
    }

    // Step 4: Map the stack into the kernel virtual address space
    uint64_t stack_virt = alloc_stack_vaddr(STACK_PAGES);
    uint64_t stack_size = STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < STACK_PAGES; i++) {
        uint64_t phys = stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] TaskBuilder::build: stack map failed at page %u\n", i);
            delete task;
            return nullptr;
        }
    }
```

`alloc_stack_vaddr` 从一个单调递增的内核虚拟地址（`0xFFFF800000100000` 起）分配连续的虚拟页。物理内存通过 PMM 分配 4 个连续物理页，然后逐页映射到内核虚拟地址空间。`0x03` 标志是 `FLAG_PRESENT | FLAG_WRITABLE`。如果任何一页映射失败，函数会释放已分配的 TCB 并返回空指针。

```cpp
    // Step 5: Write stack overflow magic at the very bottom
    *reinterpret_cast<uint64_t*>(stack_virt) = STACK_MAGIC;

    // Step 6: Initialise the CPU context
    task->ctx.rsp = stack_virt + stack_size - 8;
    *reinterpret_cast<uint64_t*>(task->ctx.rsp) =
        reinterpret_cast<uint64_t>(&cinux::proc::Scheduler::exit_current);
    task->ctx.rip = reinterpret_cast<uint64_t>(entry_);
    task->ctx.r15 = 0;
    task->ctx.r14 = 0;
    task->ctx.r13 = 0;
    task->ctx.r12 = 0;
    task->ctx.rbp = 0;
    task->ctx.rbx = 0;
```

这段是整个 `build()` 函数中最精巧的部分。`ctx.rip` 指向线程入口函数，但 `ctx.rsp` 不是简单的栈顶——它指向 `stack_virt + stack_size - 8`，并且在那个位置写入了 `Scheduler::exit_current` 的地址。这是因为在 x86-64 的调用约定中，`ret` 指令从栈顶弹出一个 8 字节值作为返回地址。当 `context_switch` 通过 `jmp *56(%rsi)` 跳到入口函数时，不是 `call`，所以没有在栈上压入返回地址。线程函数执行到最后的 `ret` 时，它会弹出我们预先放置的 `exit_current` 地址，从而进入干净的退出流程。这个设计类似于 PintOS 中 `kernel_thread()` 封装调用 `thread_exit()` 的模式。

```cpp
    // Step 7: Fill in the remaining task fields
    task->state            = TaskState::Ready;
    task->tid              = next_tid++;
    task->priority         = priority_;
    task->kernel_stack     = stack_virt;
    task->kernel_stack_top = stack_virt + stack_size;
    task->addr_space       = addr_space_;
    task->sched_class      = sched_class_;
    task->name             = name_;

    cinux::lib::kprintf("[PROC] Created task tid=%u name='%s' stack=0x%p\n",
                        task->tid, task->name,
                        reinterpret_cast<void*>(task->kernel_stack_top));

    return task;
```

最后填充 TCB 的元数据字段。`tid` 通过 `next_tid++` 分配，初始状态设为 `Ready`，`addr_space` 为 nullptr 表示纯内核线程，`sched_class` 为 nullptr 时由 `Scheduler::add_task()` 自动分配默认的 RoundRobin 策略。

## 设计决策

**CpuContext 为什么用 static_assert 而不直接用数组？** 用命名字段 + `static_assert` 是显式的文档——每个字段名就是它的语义，同时编译期断言保证了与汇编层的偏移量一致。如果用 `uint64_t regs[8]` 加宏定义偏移量，可读性会大打折扣，而且很容易在新增字段时忘记更新宏。这套 "声明即文档、编译即校验" 的做法和 Linux 的 `pt_regs` 设计理念一脉相承。

**Task 结构体为什么保持精简？** 当前的 `Task` 只有 9 个字段——调度、内存、标识所需的核心信息。对比 Linux 的 `task_struct`（超过 9 KB），Cinux 选择了"够用就好"的策略：先实现多线程调度所需的最小集合，后续文件描述符、进程树、工作目录等功能在对应的 tag 中逐步添加到 `Task` 中。这样每个 tag 的变更范围可控，读者也更容易跟踪 `Task` 结构体的演化过程。

**栈为什么用魔数检测溢出？** `STACK_MAGIC` 写在栈底，如果线程的栈指针增长到覆盖了栈底的魔数，调试时通过检查魔数是否被破坏就能判断是否发生了栈溢出。更完善的方案是再加一个 guard page（未映射的保护页），但当前的魔数方案已经能覆盖最常见的栈溢出场景，且不需要额外的虚拟地址空间开销。

## 扩展方向

- **优先级调度**：`priority` 字段已预留，可以实现 Priority RoundRobin 或简单的 O(1) 调度器。
- **per-CPU 运行队列**：当前 `Scheduler` 是全局唯一的，扩展到 SMP 时需要每个 CPU 持有独立的 `RoundRobin` 实例。
- **Guard page**：在栈底下方预留一页未映射的虚拟地址作为溢出保护页，栈增长越界时触发 #PF。

## 参考资料

- Intel SDM Vol. 3A, Chapter 8 "Task Management", Section 8.7 "Task Management in 64-Bit Mode" (PDF pages 281-282): 64 位模式不支持硬件任务切换
- System V AMD64 ABI: https://gitlab.com/x86-psABIs/x86-64-ABI — callee-saved 寄存器定义
- xv6 `proc.h` / `swtch.S`: https://github.com/mit-pdos/xv6-public — 对比 TCB 和上下文切换设计
- PintOS `switch.S`: https://uchicago-cs.github.io/mpcs52030/switch.html — 线程退出封装模式
