# 020-2 抢占式调度核心实现

## 导语

上一篇我们把 Spinlock、PerCPU 和 tss_set_rsp0 这些基础设施搭好了。现在终于到了整个 tag 020 最核心的部分——实现抢占式调度。我们要做的工作包括：让定时器中断驱动调度器的时间片管理（tick），实现真正的抢占式任务切换（schedule），创建一个什么都不干只执行 hlt 的 idle 任务，以及为未来的 Mutex/Semaphore 打好地基的 block/unblock 机制。

从协作式到抢占式的转变，本质上就是从"线程主动让出 CPU"变成"定时器来抢走 CPU"。这个转变听起来只是换了个触发源，但实际上它改变了整个调度器的调用上下文——yield() 是在普通函数调用中触发的，而 tick() 是在中断处理程序中被调用的。这个区别会在下一篇的 IF 标志修复中引发一个大坑，但这一篇我们先专注于调度逻辑本身的正确性。

## 概念精讲

### 时间片轮转——为什么是 2 个 tick

时间片（time slice 或 quantum）是抢占式调度中最基本的概念：每个任务被分配一段固定长的 CPU 时间，时间用完后调度器强制切换到下一个任务。OSDev Wiki 的 Scheduling Algorithms 页面推荐时间片在 20ms 到 50ms 之间——太短了上下文切换开销占比太高（每次切换要保存/恢复寄存器、刷新 TLB、可能还有 cache miss），太长了交互响应迟钝。

我们的 PIT 配置为 100Hz，每个 tick 是 10ms。时间片设为 DEFAULT_TIME_SLICE = 2，也就是 20ms 一次抢占。这个值在 QEMU 模拟环境下经过实测是合适的——如果设成 10（100ms），三个线程各自跑完自己的忙循环都用不了 100ms，抢占根本不会发生。这个教训记录在 `document/notes/020/001_time_slice_too_long.md` 中，下一篇会详细讲。

### tick() 与 schedule() 的分工

我们把"是否该切换"和"怎么切换"拆成两个函数，这是有意的分层设计。tick() 是定时器中断的回调——它负责计数和判断：当前任务已经跑了几个 tick 了？有没有超过时间片？如果超过了，它重置计数器并调用 schedule()。schedule() 是真正的切换逻辑——它不管"为什么切换"，只管"切换到谁"以及"怎么切换"。

这种分工让 schedule() 可以被其他场景复用——block() 在阻塞当前任务时也调用 schedule()，yield() 的实现也是直接调用 schedule()。如果你把计数逻辑和切换逻辑耦合在一起，每次调用点都得重复写判断代码，维护起来会非常痛苦。

### idle 任务——CPU 不能停下来

当所有真实任务都处于 Blocked 或 Dead 状态时，调度器需要一个"兜底"任务来执行——否则 pick_next() 返回 nullptr，调度器不知道该怎么办了。idle 任务就是这个兜底：它永远处于 Ready 状态（实际上不加入 RoundRobin 队列），永远执行 `hlt` 指令让 CPU 进入低功耗等待状态。中断到来时 CPU 自动醒来，处理完中断后回到 hlt 继续等待。

idle 任务的优先级设为 255——最低。在当前只使用 RoundRobin 的场景下优先级没有实际作用，但这是为将来实现优先级调度做的预留。idle 任务也不加入 RoundRobin 队列，而是在 schedule() 中 pick_next() 返回 nullptr 时作为备选被选中。如果连 idle 都没有（理论上不应该发生），调度器就执行 `cli; hlt` 永久停机。

### block/unblock——为同步原语铺路

block 和 unblock 是比 Spinlock 更高层次的同步机制。Spinlock 是"忙等"——拿不到锁就原地转圈，CPU 时间全浪费了。block 的思路是"拿不到锁就睡觉"——把任务标记为 Blocked、从运行队列中移除，CPU 去执行别的任务。等锁可用了再 unblock——把任务重新标记为 Ready、放回运行队列。

block() 接受一个 reason 字符串参数，纯粹是用于日志调试——当你看到 `[SCHED] Task tid=3 'consumer' blocked: semaphore_wait` 这样的日志时，立刻就知道哪个任务因为什么原因被阻塞了。在内核调试中，这种信息是无价的——死锁排查的第一步永远是"谁在等什么"。

unblock() 除了恢复状态还要重新入队——而且要确保任务有调度策略。如果一个任务在创建时没有指定调度策略，unblock 会把默认的 RoundRobin 分配给它，防止任务回到运行队列后没有 sched_class 指针导致空指针崩溃。

## 动手实现

### Step 1: 实现 idle 任务

**目标**: 在 Scheduler 中创建 idle 任务及其入口函数。

**设计思路**: idle 任务的入口函数 `idle_entry()` 极其简单——一个无限循环，循环体只有一条 `hlt` 指令。hlt 让 CPU 进入 HALT 状态，功耗降到最低，同时保持中断响应能力——当 IRQ0 定时器中断到来时 CPU 自动醒来，执行完中断处理后回到 hlt。

idle 任务在 Scheduler::init() 中创建，使用 TaskBuilder 设置入口为 idle_entry、名字为 "idle"、优先级为 255。创建后手动将状态设为 Ready（因为 idle 任务不通过 add_task 加入 RoundRobin 队列，所以不会在 add_task 中被设为 Ready）。

**实现约束**: idle_entry 是一个 static 私有方法，不暴露给外部。idle_task_ 静态成员保存指针，init() 中分配，后续 schedule() 和 exit_current() 中使用。

**踩坑预警**: idle 任务千万不要调用 add_task()——如果把它加入 RoundRobin 队列，pick_next() 就有可能选到它，即使在有其他就绪任务的情况下。idle 任务应该只在"没有其他选择"时才被调度。

**验证**: 编译通过。

### Step 2: 实现 tick()——时间片计数与抢占触发

**目标**: 实现 Scheduler::tick()，由定时器中断在每次 IRQ0 时调用。

**设计思路**: tick() 的逻辑非常直接。第一步检查前置条件——调度器必须已初始化（initialized_ 为 true），当前必须有正在运行的任务（current_ 不为 null）。如果不满足，直接返回。第二步递增两个计数器：tick_count_ 是全局的总 tick 计数，current_slice_ 是当前任务在本轮时间片中已经消耗的 tick 数。第三步检查时间片是否用完：如果 current_slice_ >= DEFAULT_TIME_SLICE（2），重置 current_slice_ 为 0 并调用 schedule() 进行任务切换。

这里有一个微妙的设计选择：tick() 中调 schedule() 时，我们仍然处于中断上下文——IRQ0 的 ISR 正在执行。这意味着 schedule() 中的 context_switch 会从中断处理程序内部发生，被切出的任务稍后会通过 IRETQ 路径恢复执行。这个调用链下一篇会仔细分析。

**实现约束**: tick() 是静态 public 方法。不需要任何参数——它操作的都是 Scheduler 的静态状态。先检查 initialized_ 和 current_，再做计数和判断。

**踩坑预警**: tick() 中的 schedule() 调用是抢占式调度的核心路径。如果这里的逻辑有 bug，最常见的症状是"只有第一个被抢占的任务能恢复正常，后续任务全部顺序执行"——这正是 IF 标志丢失的典型表现。那个 bug 不在 tick() 本身，而在 context_switch.S 中，下一篇会详细讲。

**验证**: 编译通过。运行验证需要等 PIT 集成完成后才能做。

### Step 3: 实现 schedule()——抢占式任务切换

**目标**: 实现 Scheduler::schedule()，这是抢占式调度的核心切换逻辑。

**设计思路**: schedule() 的执行流程分为几步。第一步，保存当前任务指针到局部变量 prev。第二步，如果 prev 的状态是 Running，把它改回 Ready——因为它马上要让出 CPU 了。第三步，调用 default_rr_.pick_next() 获取下一个应该执行的任务。

如果 pick_next() 返回 nullptr 或者返回的就是 prev 自己，说明没有其他就绪任务。这时候要区分两种情况：如果 prev 不是 Blocked 也不是 Dead（它只是暂时没有别人可以替代），就继续运行 prev——把状态改回 Running，直接返回，不做 context_switch。如果 prev 是 Blocked 或 Dead（比如刚调用了 block()），就必须切换到 idle 任务了——因为 prev 已经不能继续跑了。

如果 pick_next() 返回了一个有效的不同任务 next，就开始真正的切换：更新 current_ 为 next、更新 g_per_cpu.current、重置 current_slice_ 为 0。如果 next 不是 idle 任务，调用 GDT::tss_set_rsp0(next->kernel_stack_top) 更新 TSS。最后调用 context_switch(&prev->ctx, &next->ctx) 完成实际的寄存器和栈切换。

**实现约束**: schedule() 是静态 public 方法。必须使用局部变量 prev 保存旧的 current_ 值，不能直接用 current_ 作为 context_switch 的 from 参数——因为 current_ 在调用 context_switch 之前就被更新成了 next，如果用 current_->ctx 做 from，context_switch 会把新任务的寄存器保存到新任务的 CpuContext 里，变成一个昂贵的 no-op。

**踩坑预警**: 这里有一个 tag 019 就提到的经典 bug——指针覆盖。如果你写成 `current_ = next; context_switch(&current_->ctx, &next->ctx)`，from 和 to 就指向同一个 Task，context_switch 什么都没做，执行继续在已经让出 CPU 的任务栈上跑——这会导致各种诡异的崩溃。必须先 `Task* prev = current_;` 再 `current_ = next;`。

**验证**: 编译通过。

### Step 4: 实现 block() 和 unblock()

**目标**: 实现任务的阻塞和唤醒机制。

**设计思路**: block() 的流程是这样的：把任务状态设为 Blocked，从其调度策略的运行队列中 dequeue 移除，打印一条日志（包含 reason 参数）。关键的一步是——如果被阻塞的是当前正在运行的任务（task == current_），必须立刻调用 schedule() 让出 CPU，因为当前任务已经不能继续跑了。如果阻塞的是别的任务（比如一个子线程被父线程阻塞），只需要标记状态并移除队列即可，不需要切换。

unblock() 是 block() 的逆操作：把任务状态设回 Ready，确保它有调度策略（没有的话分配默认的 RoundRobin），调用 enqueue 重新加入运行队列，打印日志。unblock 不需要调用 schedule()——被唤醒的任务会在下一个 tick 或者下次 schedule() 时被 pick_next() 选中。

**实现约束**: block 和 unblock 都是静态 public 方法。block 的 reason 参数可以为 null（调用时传 nullptr），日志中会打印 "unknown"。unblock 中检查 sched_class 是否为 null 是防御性编程——正常情况下 add_task 已经设置了 sched_class，但万一有代码路径跳过了 add_task，这里不会崩溃。

**验证**: 编译通过。

### Step 5: 实现 remove_task() 和 is_initialized()

**目标**: 补充两个辅助方法。

**设计思路**: remove_task() 是一个显式的任务移除操作——从调度策略的队列中 dequeue，然后设置状态为 Dead。和 exit_current() 不同，remove_task 可以移除任何任务，不仅仅是当前任务。目前它主要用于测试，未来会用于进程终止（kill）的实现。

is_initialized() 就是一个 getter——返回 initialized_ 的值。PIT 的 IRQ0 handler 在调用 tick() 之前会检查这个值，防止在调度器初始化完成之前就收到定时器中断。

**实现约束**: remove_task 检查 task 指针是否为 null，检查 sched_class 是否为 null（dequeue 前要确认调度策略存在）。is_initialized 直接返回 bool。

**验证**: 编译通过。

### Step 6: 更新 init() 和现有的 yield()/exit_current()/run_first()

**目标**: 让现有的调度器方法适配新增的抢占式逻辑。

**设计思路**: init() 中增加了 idle 任务的创建和 initialized_ 标志的设置。idle 任务在 init() 末尾创建，创建后手动设置状态为 Ready 并打印日志。initialized_ = true 放在最后——这意味着只有所有初始化步骤完成后，tick() 才会真正执行调度逻辑。

yield() 的实现简化了——它现在只需要调用 schedule()。在 tag 019 中 yield() 自己实现了 pick_next 和 context_switch 的逻辑，现在这些都统一到 schedule() 中了。

exit_current() 中增加了 idle 任务的 fallback——如果 pick_next() 返回 nullptr，不再直接 halt，而是切换到 idle 任务（如果存在的话）。同时也增加了 g_per_cpu.current 和 tss_set_rsp0 的更新。

run_first() 中增加了 g_per_cpu.current 和 tss_set_rsp0 的初始化——在第一次切换到任务之前就要设置好这两个值。

**实现约束**: init() 中的初始化顺序很重要——先清空所有状态（包括新增的 idle_task_、tick_count_、current_slice_），再注册调度策略，然后创建 idle 任务，最后设置 initialized_ = true。

**验证**: 编译通过。

### Step 7: 编译验证

**目标**: 确认所有修改编译通过。

```bash
cmake --build build --target big_kernel 2>&1 | tail -30
```

如果编译通过，恭喜你——抢占式调度的核心逻辑已经完成了。不过如果你现在就运行，什么都不会发生——因为 PIT 的 IRQ0 handler 还没有接入 tick()，定时器中断来了也调不到调度器。这个集成工作在下一篇的第一步中完成。

## 构建与运行

到这一步我们实现了 tick()、schedule()、block/unblock、remove_task、idle 任务和 is_initialized，并且更新了 init()、yield()、exit_current() 和 run_first()。但是内核的行为仍然和 tag 019 一样——因为 tick() 从来没有被调用过。PIT 的中断还是在空跑，每一次 IRQ0 都只是递增一下计数器然后 EOI 返回，调度器完全不知道定时器的存在。

下一篇我们会把 PIT 和 tick() 连起来，然后修复一个让笔者血压拉满的 bug——context_switch 中丢失 IF 标志的问题。修复之后你就能看到多个线程在没有任何 yield 调用的情况下被定时器中断强制交替执行了。

## 调试技巧

**schedule() 切换后系统卡死，没有任何输出**: 最可能的原因是 context_switch 的 from 和 to 指向了同一个 Task。在 schedule() 中加一个 kprintf 打印 prev 和 next 的地址和 tid，确认它们确实不同。

**idle 任务被反复调度，真实任务永远得不到 CPU**: 说明真实任务没有正确地进入 RoundRobin 队列。检查 add_task() 是否被正确调用，以及 schedule() 中 prev->state 的判断是否正确——如果 prev 的状态不是 Running（比如已经是 Ready），schedule() 可能会错误地认为不需要切换。

**block() 后系统卡死**: 如果你 block 了当前任务但没有调用 schedule()，当前任务继续以 Blocked 状态运行，下次 tick() 发现 current_->state 不是 Running 就跳过——然后 prev->state != Blocked 的判断也不会触发 idle 切换。确保 block() 中 `if (task == current_) schedule();` 这一行没有被漏掉。

**tick_count_ 在增长但 schedule() 从未被调用**: 检查 current_slice_ 是否在 tick() 中被正确递增，以及 DEFAULT_TIME_SLICE 的值是否正确。如果 DEFAULT_TIME_SLICE 被意外设成了很大的值（比如 100），那就要等 100 个 tick（1 秒）才会触发一次调度。

## 本章小结

这一篇我们把抢占式调度的核心逻辑全部实现了：tick() 在每次定时器中断时递增时间片计数器并在用完时触发调度，schedule() 实现了真正的抢占式任务切换（包括 idle fallback 和 PerCPU/TSS 更新），block/unblock 提供了任务阻塞和唤醒的基础机制（为 021 的 Mutex/Semaphore 铺路），idle 任务确保了 CPU 永远有事可做。但这一切目前都是"静默"的——tick() 从来没被调用过。下一篇我们要做的第一件事就是把 PIT 的 IRQ0 和 tick() 连起来，然后修复 context_switch 中丢失 IF 标志的经典 bug，让抢占式调度真正运转起来。
