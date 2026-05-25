---
title: 019-proc-context-3 · 进程上下文
---

# 踩坑实录：两个让内核崩掉的 Bug

> 标签：调试, Higher-Half, 线程退出, 栈溢出, 上下文切换, triple fault
> 前置：[019-2 一帧汇编搞定上下文切换](019-proc-context-2.md)

## 前言

前两章我们把调度器"应该怎样工作"讲清楚了——数据结构怎么设计、汇编原语怎么写、调度策略怎么选。现在该讲点真实的东西了：我实际上把这些代码跑起来的时候，到底发生了什么。

说实话，tag 019 是整个 Cinux 项目中 debug 体验最"精彩"的 tag 之一（虽然当时血压拉满）。这个 tag 里我们踩了三个坑：第一个是 higher-half 内核的 ELF 入口点地址错误，第二个是线程函数返回时栈上没有返回地址，第三个是 `exit_current` 里的指针覆盖。后两个 bug 叠在一起，表现形式是 QEMU 报 `emulation failure, RIP=0xDEADC0DE`，thread_b 没跑完就直接 triple fault——看起来像是完全不相干的两件事，实际上根因串在一起。这一章我们就一个一个拆。

## Bug 1：Higher-Half 内核跑在了恒等映射上

### 现象

这个 bug 的表现很隐蔽——内核能正常启动，打印信息、初始化子系统，看起来一切正常。直到我们开始创建 `AddressSpace` 的时候，才发现不对劲：两个独立的 `AddressSpace` 之间发生了页表项"泄漏"，在一个地址空间中创建的映射居然出现在了另一个地址空间里。进程隔离？不存在的。

### 根因

问题出在 mini kernel 的 ELF 加载器里。大内核的 ELF 入口点在链接时就是 higher-half 虚拟地址（`0xFFFFFFFF81000000`），引导加载程序已经在 higher-half 映射中放置了所有物理内存。所以 `saved_entry` 本身就是一个可以直接跳转的虚拟地址——不需要任何转换。但加载器的代码做了画蛇添足的操作：

```cpp
// elf_loader.cpp — 修改前（错误）
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = saved_entry;
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;  // 0xFFFFFFFF81000000 → 0x1000000
}
return entry;
```

它把 higher-half 地址减去偏移量，转成了物理地址 `0x1000000`。引导时设置了两套映射，指向同一个 PDPT/PD 子树：`PML4[0]` 指向恒等映射的 PDPT，`PML4[511]` 指向 higher-half 的 PDPT。`identity_map_up_to()` 填充 PD 表项时，自动覆盖了两条路径——所以恒等映射和 higher-half 映射都能正常工作。加载器把入口点指向恒等映射地址后，大内核就跑在 `PML4[0]` 的世界里了。

这本身不是致命问题——恒等映射能正常工作嘛。但连锁反应来了：`AddressSpace` 构造函数原本会复制 `PML4[0]`（恒等映射条目）到新创建的地址空间。因为所有进程都复制了同一个 `PML4[0]`，它们共享了同一套恒等映射的 PDPT/PD 子树。一个进程在这个共享子树中修改的页表项会"泄漏"到其他进程的地址空间，进程隔离被彻底破坏了。

### 修复

修复方案是双管齐下。首先 ELF 加载器直接返回 `saved_entry`（higher-half 地址），不做任何偏移转换——就这一行改动，让大内核跑在了正确的 higher-half 地址空间里。然后 `AddressSpace` 构造函数不再复制 `PML4[0]`，只复制 `PML4[256..511]`（内核 higher-half 部分）：

```cpp
// address_space.cpp — 修改后
auto* kern_pml4 = phys_to_virt(kernel_pml4_);
for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
    pml4[i].raw = kern_pml4[i].raw;
}
```

`USER_PML4_END = 256`，只复制后半部分。每个进程的 `PML4[0]` 都是空的，用户空间的映射需要由进程自己通过 `map()` 建立。这个修复顺带还让之前的测试 workaround 全部可以移除了——隔离测试现在对任意地址都能正常工作，不需要什么内联汇编技巧。

## 大页拆分：为线程栈铺路

Higher-half 修复之后，还有一个不那么起眼但同样关键的问题需要解决。引导阶段 `identity_map_up_to()` 为了效率用了 2MB 大页（huge page），这本身没问题。但当 `TaskBuilder::build()` 试图为线程栈映射 4KB 页时，如果目标虚拟地址恰好落在一个 2MB 大页范围内，原来的 `walk_level()` 会直接返回那个大页的物理地址——它不知道怎么在一个大页内部的特定 4KB 偏移处创建映射。

修复是在 `walk_level()` 中加大页检测和拆分逻辑。当遇到一个 `entry.huge` 标志且 `should_alloc` 为 true 时（说明调用者想要修改页表而不只是查找），函数会分配一个新的页表（PT），把原来的 2MB 大页拆成 512 个 4KB 页表项——每个 PTE 指向 `big_phys + i * PAGE_SIZE`，标志位沿用大页的标志但去掉 `FLAG_HUGE`。然后把原来的大页条目替换为指向新 PT 的普通页表项。这个操作对调用者完全透明：`g_vmm.map()` 不需要知道自己操作的目标地址原本是不是大页。

拆分逻辑放在 `walk_level` 里而不是独立函数中，是因为 `walk_level` 是所有页表遍历的必经之路。不管是 `map()`、`unmap()` 还是 `translate()` 都通过它逐级下降，在这里做拆分保证了全局一致性。当然，这个简化实现在多地址空间共享大页的场景下会有问题——拆分只影响当前页表，不通知其他地址空间。不过对于当前的 Cinux 来说，内核线程共享内核地址空间，这不是问题。

## Bug 2：线程退出时跳到了 0xDEADC0DE

### 现象

修完 higher-half 问题后，调度器终于能跑了。两个线程交替打印了 5 轮，一切看起来很美好。然后 `thread_a` 执行到最后一条 `kprintf("[A] thread_a done\n")`，函数返回——QEMU 报了 `emulation failure, RIP=0xDEADC0DE`。thread_b 还没来得及跑完，内核直接 triple fault。

`0xDEADC0DE`？这不是我们写在栈底的溢出检测魔数吗？为什么 RIP 会跑到那里去？

### 根因分析

线索就在 `0xDEADC0DE` 里。这个魔数写在内核栈的最底部——`TaskBuilder::build()` 中的 `*reinterpret_cast<uint64_t*>(stack_virt) = STACK_MAGIC`。它的作用是检测栈溢出：如果线程的栈被写穿了，CPU 最终会访问到栈底的这个值。但这里是另一种情况——RIP 不是因为栈溢出跳到这里的，而是因为 `ret` 弹出了这个值作为返回地址。

让我们回到线程的启动过程。`context_switch` 通过 `jmp *56(%rsi)` 跳到线程入口函数。注意这里是 `jmp`，不是 `call`。`call` 会在栈上压入返回地址，但 `jmp` 不会。所以当 `thread_a()` 执行到最后的 `}`（编译成 `ret` 指令）时，CPU 从 RSP 指向的位置弹出一个 8 字节值作为返回地址——但栈顶是空的。新线程的 RSP 被设为 `stack_virt + stack_size`（裸栈顶），栈上什么都没有。`ret` 弹啊弹，一路往下走，最终弹到了栈底的 `0xDEADC0DE`，CPU 试图执行地址 `0xDEADC0DE` 处的指令，QEMU 直接报 emulation failure。

这是一个典型的"调用约定不匹配"问题。C 编译器生成的函数默认假设栈顶有一个返回地址——这是 `call` 指令的语义保证。但我们的 `context_switch` 用 `jmp` 跳过去，绕过了这个保证。线程函数的 prologue/epilogue 是编译器生成的，它不知道自己被 `jmp` 而不是 `call` 调用了，所以 `ret` 毫无防备地弹了垃圾。

### 修复：在栈上预压退出地址

修复方法我们在第一篇提过，但这里再仔细看一下为什么这样修是正确的：

```cpp
// process.cpp — 修复后
task->ctx.rsp = stack_virt + stack_size - 8;
*reinterpret_cast<uint64_t*>(task->ctx.rsp) =
    reinterpret_cast<uint64_t>(&Scheduler::exit_current);
task->ctx.rip = reinterpret_cast<uint64_t>(entry_);
```

RSP 从裸栈顶下移 8 字节，在这个位置写入 `exit_current` 的地址。现在栈顶有一个有效的"返回地址"了。线程函数执行 `ret` 时，弹出的是 `exit_current`，CPU 跳到 `Scheduler::exit_current()`，由它负责把当前线程标记为 Dead、从运行队列中移除、然后切换到下一个线程。从线程函数的视角看，就好像它被一个"看不见的调用者"调用了——这个调用者的返回地址指向退出处理函数。

这个设计不是 Cinux 独创的。PintOS 用的是另一种等价方案：每个新线程不是直接从用户函数开始跑，而是从一个叫 `kernel_thread()` 的 wrapper 开始。`kernel_thread()` 的伪代码大概是 `call user_function; call thread_exit()`——用户函数返回后，执行流自然地落到 `thread_exit()` 调用上。Cinux 的方案更直接：不用 wrapper 函数，而是把退出地址直接压在栈上。两种方案本质上做的是同一件事——保证线程函数返回后有一个合法的去处，而不是掉进深渊。

但这里有一个值得思考的 trade-off。PintOS 的 wrapper 方案多了一层函数调用（`kernel_thread` 调用用户函数，多了一个栈帧），但它的语义非常清晰——线程从 `kernel_thread` 开始，用户函数是正常的函数调用，返回后继续执行 `thread_exit`，一切都符合 C 的调用约定。Cinux 的方案更轻量（少一层调用），但它依赖一个微妙的前提：`context_switch` 用 `jmp` 跳到入口函数后，栈顶必须预先放好退出地址。如果有人修改了 `TaskBuilder` 或者 `context_switch` 时忘了这个前提，bug 就会回来。两种方案都可以，关键是选择了哪种就要确保对应的约束在整个代码库中被正确维护。

## Bug 3：exit_current 的指针覆盖

### 现象

修完 Bug 2 之后（在栈上压了 `exit_current` 地址），我以为应该没问题了。重新编译，运行，两个线程交替打印 5 轮，`thread_a` 打印 done，进入 `exit_current`——然后又崩了。这次的崩溃方式更诡异：线程没有退出，而是继续在已退出的线程栈上执行，最终在某个不确定的时刻 triple fault。

### 根因分析

这次的线索要仔细看。崩溃发生在 `exit_current()` 内部。让我展示一下修复前的代码：

```cpp
// Scheduler::exit_current() — 修改前（有 bug）
current_ = next;                           // current_ 被覆盖了！
context_switch(&current_->ctx, &next->ctx); // from == to
```

看到了吗？`current_ = next` 把当前指针设为下一个线程，然后 `context_switch(&current_->ctx, &next->ctx)`——此时 `current_` 和 `next` 指向同一个 `Task`，`from` 和 `to` 是同一个 `CpuContext`。`context_switch` 做了什么呢？保存当前寄存器到 `from`，从 `to` 恢复寄存器——但 `from` 和 `to` 是同一块内存。所以它先把自己的状态保存进去，然后又从自己恢复回来。一个完美的空操作。线程根本没有切换走，继续在已退出的 `thread_a` 的栈上执行。

你可能想问：`prev` 已经被标记为 `Dead` 了，`pick_next()` 返回的是 `next`（比如 `thread_b`），为什么切换还是失败？因为问题不在"选了谁"，而在"传给 `context_switch` 的参数是不是正确的"。`from` 应该是当前正在退出的线程（`prev`）的上下文，`to` 应该是下一个线程（`next`）的上下文。但代码把 `current_` 改成了 `next` 之后，`from` 变成了 `next` 的上下文，和 `to` 一样——切换变成了自己切自己。

这个 bug 非常隐蔽，因为它不是逻辑错误——逻辑上"先设 current_ 再切换"看起来很合理。它是顺序依赖错误：先改了指针再用指针，导致指针已经不指向原来的对象了。而且它不会立刻 crash——`context_switch` 的空操作不会触发任何异常，线程只是"假装切换了"然后继续执行。直到已退出的线程栈被踩了、或者某个不相关的操作触发了异常，问题才暴露出来。这种"延迟发作"的 bug 是内核调试中最让人头疼的类型。

### 修复：先存后改

修复方法简单到让人想抽自己——用局部变量保存旧值：

```cpp
void Scheduler::exit_current() {
    Task* prev = current_;          // 先保存！
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
    }

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        while (1) __asm__ volatile("cli; hlt");
    }
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);  // from != to，正确切换
}
```

`prev` 是在修改 `current_` 之前保存的，所以它始终指向正在退出的线程。`context_switch(&prev->ctx, &next->ctx)` 中 `from` 是退出的线程、`to` 是下一个线程——两个不同的指针，切换正常执行。

如果你回头看 `schedule()` 函数，你会发现它一开始就用了 `Task* prev = current_;`——那里写对了，但 `exit_current()` 里忘了。同一个文件、同一个模式、两处代码，一处写对了另一处写错了。这种"复制粘贴忘了改"的错误在内核开发中太常见了。

## 全部修完后的串口输出

三个 bug 全部修完后，串口输出终于变成了我们期望的样子：

```
[A] thread_a iteration 0
[B] thread_b iteration 0
[A] thread_a iteration 1
[B] thread_b iteration 1
[A] thread_a iteration 2
[B] thread_b iteration 2
[A] thread_a iteration 3
[B] thread_b iteration 3
[A] thread_a iteration 4
[B] thread_b iteration 4
[A] thread_a done
[SCHED] Task tid=1 'thread_a' exited
[B] thread_b done
[SCHED] Task tid=2 'thread_b' exited
[SCHED] No more tasks, halting.
```

两个线程各跑 5 轮，交替打印。`thread_a` 先完成，通过 `exit_current` 干净退出。`thread_b` 接着跑完，同样干净退出。调度器发现运行队列空了，停机。没有 crash，没有 triple fault，没有 `0xDEADC0DE`。虽然看起来就是十几行输出，但背后的 debug 过程远比这几行精彩。

## 附带修复：demand-page 的 CR3 感知

在修这些 bug 的过程中，顺便还修了一个和进程隔离相关的问题。原来的页错误处理函数在 demand-page 时直接调用 `g_vmm.map(virt_page, phys, flags)`，不传 CR3 参数，默认使用全局内核页表。在单地址空间时代这不是问题——所有线程共享同一个 PML4。但引入进程后，每个进程有独立的 CR3，demand-page 必须在当前进程的页表中进行映射：

```cpp
// exception_handlers.cpp — handle_pf 修改后
uint64_t cur_cr3 = cinux::arch::read_cr3();
bool ok = g_vmm.map(virt_page, phys,
                    FLAG_PRESENT | FLAG_WRITABLE, &cur_cr3);
```

先读 CR3 获取当前页表的物理地址，然后传给 `g_vmm.map()` 作为目标页表。如果继续使用全局页表，demand-page 创建的映射会出现在错误的地址空间中——这种 bug 更难排查，因为它只在特定进程触发缺页时才暴露，而且表现是"内存映射不生效"而不是直接 crash。

## 环境说明

本章不引入新的核心文件——所有代码修改都发生在已经存在的文件中。bug 修复涉及的文件是 `kernel/mini/elf_loader.cpp`（higher-half 修复）、`kernel/mm/vmm.cpp`（大页拆分）、`kernel/mm/address_space.cpp`（PML4 复制范围调整）、`kernel/proc/process.cpp`（栈上压入退出地址）、`kernel/proc/scheduler.cpp`（`exit_current` 指针修复）以及 `kernel/arch/x86_64/exception_handlers.cpp`（demand-page CR3 感知）。调试环境是 QEMU + 串口输出——没有 GDB（因为 bug 涉及上下文切换，断点设在哪里都不太方便），主要靠打印和推理。

## 回头看：这些 bug 教会了我们什么

这三个 bug 的共同特点是：它们都不是"编译报错"或者"立刻 crash"那种明显的错误。Higher-half 的 bug 让内核在错误的地址空间里正常运行了十几秒，直到 `AddressSpace` 的隔离测试才暴露。线程退出的两个 bug 叠在一起，`0xDEADC0DE` 的 RIP 看起来像是栈溢出（实际上不是），`exit_current` 的空操作让线程"假装切换了"继续跑（看起来像是正常的）。每一个都像是别的什么问题。

内核调试的经验之一就是：你看到的现象往往不是根因。RIP=`0xDEADC0DE` 不是栈溢出，而是 `ret` 弹了魔数；线程继续执行不是"yield 没生效"，而是 `context_switch` 变成了空操作。找到根因的关键是回到"这一步到底做了什么"而不是"看起来发生了什么"。对于上下文切换这种涉及寄存器和栈指针的操作，最好的办法是在纸上画一遍——每条指令执行后，每个寄存器的值是什么，RSP 指向哪里，栈上有什么。这听起来很笨，但对于这类 bug 几乎是唯一可靠的方法。

另一个教训是关于 PintOS 的。PintOS 的 `kernel_thread()` wrapper 函数正是为了避免 Bug 2 而设计的——每个线程从一个 wrapper 开始执行，wrapper 调用用户函数，函数返回后 wrapper 调用 `thread_exit()`。Cinux 走了一条更"极简"的路：不用 wrapper，直接把退出地址压在栈上。这更简洁，但也更容易犯错——如果忘了压那个地址，就会跳到 `0xDEADC0DE`。两种方案都可以，但你选择了一种就必须理解它的所有后果。

## 收尾

tag 019 到这里就完整了。我们从"为什么需要多任务"开始，设计了 `Task`、`CpuContext`、`TaskBuilder`，写了 `context_switch.S` 和 `RoundRobin` 调度器，然后在调试中踩了三个坑，最终让两个内核线程漂亮地交替运行并干净退出。2264 行新增代码，471 行删除，21 个文件变更——这是 Cinux 到目前为止最大的一个 tag，也是第一个让内核从"单任务"跃升到"多任务"的里程碑。

下一个 tag 是 `020_proc_scheduler`——从合作式调度升级到抢占式调度，由时钟中断驱动自动切换。到时候就不需要手动调用 `yield()` 了。

## 参考资料

- Intel SDM Vol. 3A, Section 4.5.2：CR3 与地址空间切换
- Intel SDM Vol. 3A, Section 4.3.2：大页（2MB/1GB）的页表格式
- PintOS `kernel_thread()` wrapper: [https://uchicago-cs.github.io/mpcs52030/switch.html](https://uchicago-cs.github.io/mpcs52030/switch.html) — 线程退出封装的替代方案
- document/notes/019_proc_context/001_higher_half_fix.md：Higher-Half 修复的完整调试笔记
- document/notes/019_proc_context/002_thread_exit_crash.md：线程退出崩溃的完整排查笔记
