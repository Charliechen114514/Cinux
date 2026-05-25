---
title: 019-proc-context-3 · 进程上下文
---

# 019-3 Higher-Half 修复与集成测试

## 导语

上一篇结束时，我们的两个线程已经能在 QEMU 里交替打印了——但如果 thread_a 先执行完并 return，内核大概率会 triple fault。这不是调度器的逻辑问题，而是两个隐藏的 bug 叠在一起：线程函数没有返回地址导致 ret 跳到了 magic 值 0xDEADC0DE，加上 exit_current 里指针覆盖导致 context_switch 变成 no-op。

另外，我们还发现了一个更早就存在但一直没暴露的问题：mini kernel 的 ELF 加载器在加载大内核时，错误地把 higher-half 入口地址转换成了恒等映射地址，导致大内核运行在错误的虚拟地址上——所有 AddressSpace 实例共享 PML4[0]，进程隔离名存实亡。这一篇我们把这三个问题全部修掉，然后用一个干净的两线程交替打印测试来收尾。

本篇的内容可以分为三组独立的修复：(1) Higher-Half ELF 入口修复 + AddressSpace PML4 复制范围调整；(2) walk_level 大页拆分；(3) 线程退出崩溃的两个 bug 修复。每组修复都可以独立验证——先修 Higher-Half 确认地址空间隔离正常，再修大页拆分确认栈映射成功，最后修线程退出确认两线程能干净退出。

读完这一篇后，你应该能回答这些问题：为什么 ELF 入口地址不应该被转换？为什么 PML4[0] 不应该被复制到新的 AddressSpace？为什么线程栈上需要一个退出地址？为什么 exit_current 必须先保存 prev 再更新 current_？如果你能回答这四个问题，说明你已经完全理解了这三个 bug 的根因和修复逻辑。

## 概念精讲

### 线程退出崩溃的两个 bug

事情是这样的：我们创建两个内核线程 thread_a 和 thread_b，各循环打印 5 次，每次 yield 让出 CPU。5 轮之后 thread_a 的入口函数 return 了——然后 QEMU 输出 `emulation failure, RIP=00000000deadc0de`，thread_b 再也没机会执行。

第一个 bug 在 TaskBuilder::build() 中。初始化 CpuContext 时，rsp 被设为裸的栈顶（stack_virt + stack_size），栈上什么也没放。context_switch 通过 `jmp *56(%rsi)` 跳到线程入口函数——注意是 jmp 不是 call，所以栈上没有压入返回地址。线程函数执行到最后的 ret 指令时，ret 从栈顶弹出一个 8 字节值作为返回地址。栈顶是空的，CPU 一路弹到栈底，弹出的值是我们写入的 magic 0xDEADC0DE，然后试图跳转到这个"地址"执行——显然这是一个无效地址，QEMU 直接报告 emulation failure。

修复方案是在 build() 中把 rsp 设为栈顶减 8（腾出一个指针宽度的空间），然后在那个位置写入 exit_current 函数的地址。这样一来，线程函数 return 时 ret 弹出的就是 exit_current 的地址，执行流自动进入线程退出流程。

但事情到这里还没完。即使修了第一个 bug（在栈上放 exit_current 地址），退出时还是会崩——因为 exit_current 本身有第二个 bug。原来的代码是 `current_ = next; context_switch(&current_->ctx, &next->ctx);`。这行代码在调用 context_switch 之前就把 current_ 更新为 next 了，导致 `&current_->ctx` 和 `&next->ctx` 是同一个指针——from 和 to 指向同一个 CpuContext。context_switch 先把寄存器保存到 from，然后从 to 恢复——但 from 和 to 是同一个对象，所以恢复的其实就是刚保存的值，切换变成了 no-op。CPU 继续在已经标记为 Dead 的 thread_a 的栈上执行，迟早会访问到非法内存。

修复方案是先用局部变量保存旧指针：`Task* prev = current_;` 然后再更新 current_，最后用 prev 作为 from 参数。这个 bug 的教训是：在操作指针的时候，凡是"先赋值再用旧值"的场景都要多看一眼，特别容易出错。

值得注意的是，yield() 方法中一开始就用了 `Task* prev = current_;`——那里写对了，但 exit_current 里忘了。同一个文件、同一个模式、两处代码，一处写对了另一处写错了。这种"复制粘贴忘了改"的错误在内核开发中太常见了。

### Higher-Half 内核的 ELF 入口地址问题

这个故事更曲折。我们的大内核链接时的入口地址是 0xFFFFFFFF81000000（higher-half），但 mini kernel 的 ELF 加载器在加载大内核时，做了一件画蛇添足的事：检测到入口地址 >= HIGHER_HALF_BASE 后，把 HIGHER_HALF_BASE 减掉了。于是大内核的入口变成了 0x1000000——恒等映射地址。

这为什么是个问题？因为 bootloader 在建立页表时，恒等映射（PML4[0]）和 higher-half 映射（PML4[511]）指向了同一套 PDPT 和 PD 表。如果大内核运行在恒等映射地址上，它的页表操作（比如创建新的 AddressSpace 时复制内核 PML4 条目）就会通过 PML4[0] 的共享子树泄漏到所有 AddressSpace 实例中——进程隔离彻底失效。

修复方案出奇地简单：ELF 加载器的 load_elf 函数直接返回 saved_entry（从 ELF 头中读到的原始入口地址），不再做任何地址转换。因为 bootloader 已经建立好了 higher-half 映射，大内核的入口地址本来就应该在 higher-half——我们只需要相信链接器给出的地址就好。

同时，AddressSpace 的构造函数也需要修改：只复制 PML4[256..511]（内核 higher-half 部分），不再复制 PML4[0]（恒等映射部分）。这样每个新创建的 AddressSpace 只包含内核映射，不包含恒等映射——彻底隔离了进程间的物理页表树。

### walk_level 中的 2MB 巨大页拆分

当 VMM 的 walk_level 遇到一个 2MB huge page 条目，但需要为它分配子页表时（比如要在 2MB 页中映射一个 4KB 页），它需要把那个 2MB 页拆分成 512 个 4KB 页。具体做法是：分配一个新的 PT 页，用 512 个 4KB 条目填满它（每个条目指向原 2MB 范围内的对应 4KB 区域），然后把原来的 PD 条目从 huge page 改为指向这个新 PT 的普通条目。这个拆分操作在 Cinux 中首次出现在给内核线程分配栈映射的时候——线程栈是 4KB 粒度的映射，而内核空间的初始页表使用的是 2MB huge page。

这个拆分必须非常小心，有两个关键点：第一，拆分后必须清除原条目的 huge 标志位（bit 7），否则 CPU 仍然会把它当作 2MB 页来翻译——PTE 的解读方式在 huge 页和普通页之间完全不同。第二，拆分后需要确保 TLB 中对应 2MB 范围的缓存被刷新，否则 CPU 可能继续使用旧的 2MB 映射。在 Cinux 中，walk_level 的调用者（map_nolock）在写入新 PTE 后会调用 flush_tlb，所以这一点已经自动处理。

## 动手实现

### Step 1: 修复 TaskBuilder 栈上的返回地址

**目标**: 修改 process.cpp 中 build() 的 CpuContext 初始化逻辑，在栈顶压入 exit_current 的地址。

**设计思路**: 修改 build() 的第六步（CpuContext 初始化）。rsp 设为 `stack_virt + stack_size - 8`——注意减 8，腾出一个指针宽度的空间。然后通过 `reinterpret_cast<uint64_t*>(task->ctx.rsp)` 直接把这个 8 字节空间的内容设为 `reinterpret_cast<uint64_t>(&Scheduler::exit_current)`。rip 仍然指向线程入口函数。这样一来线程函数 return 时 ret 弹出的就是 exit_current 的地址，自动进入退出流程。

**实现约束**: 注意 rsp 的对齐——System V ABI 要求在 call 指令执行前 rsp 必须是 16 字节对齐的。不过我们的线程入口函数不是被 call 调用的（是被 jmp 跳转到的），所以对齐要求稍微宽松一些，但保持 8 字节对齐是最低要求。

**踩坑预警**: 千万别写 `rsp = stack_virt + stack_size`（不减 8）——那样的话 exit_current 的地址会被写到栈范围之外的内存里（或者 guard page 里），直接 page fault。而如果不写 exit_current 地址，ret 就会弹出 0xDEADC0DE——两种情况都会 crash，只是 RIP 不同。

**验证**: 编译并运行内核，观察 thread_a 和 thread_b 是否都能正常完成并退出。预期的行为是：thread_a 打印 done 后进入 exit_current，调度器打印 `[SCHED] Task tid=1 'thread_a' exited`，然后切到 thread_b 继续执行。如果还是 crash 在 0xDEADC0DE，说明 build() 中 rsp 的计算有问题；如果看到线程"假装切换了"（只有 thread_a 在跑），说明 exit_current 的指针覆盖还没修好。

### Step 2: 修复 exit_current 的指针覆盖

**目标**: 修改 scheduler.cpp 中 exit_current() 的实现，在更新 current_ 之前保存旧值。

**设计思路**: 在函数入口处声明 `Task* prev = current_;`，后续所有对旧任务的操作都通过 prev 指针进行（设置 state 为 Dead、dequeue）。current_ 在获取到 next 任务后才更新：`current_ = next;`。最后调用 context_switch 时使用 `context_switch(&prev->ctx, &next->ctx);`，确保 from 和 to 指向不同的 Task。

**实现约束**: 如果队列为空，使用 `cli; hlt` 死循环停机。当前是单核协作式调度，不需要 FPU 状态保存或地址空间切换。后续在支持用户态进程时需要加入 CR3 切换和 TSS RSP0 更新。

**踩坑预警**: 不仅仅是 exit_current 有这个问题——schedule() 方法也需要同样的处理。在 schedule() 中同样需要先 `Task* prev = current_;` 再更新 current_，否则同样会出现 from == to 的 no-op 切换。两个方法都要检查。

**验证**: 编译运行后，thread_a 应该能正常退出并打印 `[SCHED] Task tid=1 'thread_a' exited`，thread_b 紧随其后退出，最后输出 `[SCHED] No more tasks, halting.`。

这个 bug 的修复非常简单——一行代码的改动（把 `current_` 替换成局部变量 `prev`），但影响是全局性的。没有这个修复，内核中的所有线程都无法干净退出，任何线程退出都会导致 triple fault。这也是为什么它在 tag 019 中被归类为"关键 bug"而不是"小问题"。

### Step 3: 修复 ELF 加载器的 Higher-Half 入口地址

**目标**: 修改 kernel/mini/elf_loader.cpp 的 load_elf 函数，直接返回保存的原始入口地址。

**设计思路**: load_elf 函数在步骤 3 中已经把 ehdr->e_entry 保存到了 saved_entry。修改步骤 7 的 return 语句：直接返回 saved_entry，不再做任何 HIGHER_HALF_BASE 的减法操作。这行代码之前大概是 `return saved_entry - HIGHER_HALF_BASE` 或类似的逻辑——删掉减法即可。

**实现约束**: load_elf 的返回值类型是 uint64_t，saved_entry 也是 uint64_t，直接返回。不需要任何条件判断——链接器生成的入口地址是什么，我们就用什么。

**踩坑预警**: 这个 bug 之所以难发现，是因为"减去 HIGHER_HALF_BASE"在某些情况下是正确的——比如早期 bootloader 阶段，内核确实运行在恒等映射上。但当大内核被加载到 higher-half 地址后，它的所有符号地址（包括入口点）都已经是 higher-half 虚拟地址了。此时再减去偏移就是画蛇添足。如果你修了这个 bug 之后内核启动反而 crash 了，检查 bootloader 的页表设置是否正确建立了 higher-half 映射。

理解这个 bug 的关键在于区分"物理地址"和"虚拟地址"。ELF 入口点是虚拟地址，不是物理地址——链接器把它设为 higher-half 虚拟地址是因为内核代码应该在 higher-half 运行。引导加载程序已经建立了 higher-half 映射，所以这个虚拟地址可以直接使用，不需要任何转换。

**验证**: 编译整个项目（包括 mini kernel 和 big kernel），运行后检查大内核的启动日志。

```bash
cmake --build build && \
  qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none 2>&1 | head -5
```

输出的第一行应该显示大内核运行在 0xFFFFFFFF81000000 附近的地址，而不是 0x1000000。

### Step 4: 修复 AddressSpace 构造函数——只复制 higher-half 部分

**目标**: 修改 address_space.cpp 中 AddressSpace 构造函数的内核 PML4 条目复制逻辑。

**设计思路**: 构造函数原来可能复制了 PML4[0]（恒等映射）和 PML4[256..511]（higher-half 映射）。修改后只复制 PML4[256..511]，跳过 PML4[0]。这样新创建的 AddressSpace 不包含任何恒等映射条目，进程间的页表子树完全隔离。如果某个进程需要访问物理内存（比如内核线程），它通过 PML4[256..511] 的 higher-half 映射来访问就够了。

**实现约束**: 只是一个循环范围的修改——从复制 PML4[0..511]（或更大的范围）改为只复制 PML4[256..511]。USER_PML4_END 常量定义为 256，PT_ENTRIES 定义为 512。修改后，PML4[0] 到 PML4[255] 在新创建的 AddressSpace 中全部为零（不指向任何 PDPT），用户空间的映射需要由进程自己通过 map() 建立。

**验证**: 运行 AddressSpace 的隔离测试——创建两个 AddressSpace，在其中一个映射一个页面，验证另一个看不到这个映射。之前这个测试可能需要 workaround（因为恒等映射共享导致隔离失效），修好后应该直接通过。

这个修复顺带还让之前的测试 workaround 全部可以移除了。比如 test_address_space.cpp 中有些地方用了特殊的内联汇编技巧来绕过共享 PML4[0] 的问题，现在这些都可以删掉了——隔离测试对任意地址都能正常工作。

### Step 5: 实现 walk_level 的 2MB 巨大页拆分

**目标**: 修改 kernel/mm/vmm.cpp 中 walk_level 函数，当遇到 huge page 条目时自动拆分为 4KB 页表。

**设计思路**: 在 walk_level 的 `entry.is_present()` 分支内部，检查 `entry.huge` 标志。如果为真且 should_alloc 为真（说明调用方需要分配子表），执行拆分：先从 PMM 分配一页新的 PT，然后用 512 个 4KB 条目填满它——每个条目的物理地址 = 原 2MB 页的物理基地址 + i * PAGE_SIZE，权限位从原 huge page 条目继承（但要清除 FLAG_HUGE）。最后把原 PD 条目替换为指向新 PT 的普通条目（保留 FLAG_PRESENT | FLAG_WRITABLE | user_flag）。

**实现约束**: 拆分操作在持锁状态下执行（walk_level 的调用者通常已经持有了 VMM 的自旋锁），所以内存分配使用 PMM 的 locked 版本（alloc_page_locked）。新 PT 页通过 phys_to_virt 转换为虚拟地址后直接写入。

**踩坑预警**: 拆分后必须清除原条目的 huge 标志位，否则 CPU 仍然会把它当作 2MB 页来翻译——PTE 的解读方式完全不同（bit 7 是 PS 位，huge 页和普通页的物理地址字段位置不同）。另外，拆分后需要 flush TLB 中对应 2MB 范围的缓存，否则 CPU 可能继续使用旧的 2MB 映射——walk_level 的调用者（map_nolock）在写入新 PTE 后会调用 flush_tlb，所以这点已经自动处理了。

**验证**: 编译运行，确认内核线程的栈映射不再因 walk_level 返回 nullptr 而失败。

### Step 6: 完整集成测试——两线程交替打印

**目标**: 在 kernel_main 中完成调度器集成，验证两线程能正确交替执行并干净退出。

**设计思路**: kernel_main 的启动序列末尾添加调度器初始化和线程创建。调用 Scheduler::init()，然后用 TaskBuilder 创建两个线程（入口函数各循环 5 次打印 + yield），add_task 入队，再创建一个 boot_task 作为启动上下文，调用 run_first(boot_task) 开始调度。run_first 不会返回——它切换到第一个线程执行。当所有线程都退出后，调度器进入 halt 循环。

**验证**: 完整的预期输出：

```
[SCHED] Scheduler initialised with RoundRobin class
[SCHED] Idle task created tid=3
[SCHED] Task tid=1 'thread_a' added to RoundRobin
[SCHED] Task tid=2 'thread_b' added to RoundRobin
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

```bash
cmake --build build --target big_kernel && \
  qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none 2>&1 | grep -E '\[A\]|\[B\]|\[SCHED\]'
```

如果你看到了完整的交替输出和干净的退出信息，那么 tag 019 的所有工作都已经成功完成了。从进程数据结构到上下文切换汇编，从调度器到三个关键 bug 的修复——你的内核已经具备了协作式多线程调度的全部能力。

## 构建与运行

到这一步所有修复应该都已就位。在提交代码之前，建议做一次完整的端到端验证——从 clean build 到运行全流程：

完整的构建和验证流程如下：

```bash
# 完整构建
cmake --build build

# 运行并检查调度输出
qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none 2>&1 | tail -30
```

如果看到上面的预期输出，恭喜——你的内核已经具备了多线程调度的能力。两个线程干净地交替执行、干净地退出，没有任何 crash。

tag 019 是 Cinux 到目前为止最大的一个 tag，新增 2264 行代码，涉及 21 个文件变更。核心是五个新文件：process.hpp、process.cpp、scheduler.hpp、scheduler.cpp、context_switch.S，外加约 970 行的测试代码（QEMU 集成测试 + Host 单元测试）。

## 调试技巧

**修了 bug 之后还是 crash 在 0xDEADC0DE**: 检查 build() 中 rsp 的计算是否正确。用 kprintf 在 build() 之后打印 `task->ctx.rsp` 和 `task->ctx.rip` 的值来确认。rsp 应该比 kernel_stack_top 小 8，rip 应该是线程入口函数的地址。

**exit_current 修了但 schedule 没修**: schedule() 方法中也有同样的 prev/next 指针操作。如果 yield 触发的 schedule 中也出现了 from == to 的问题，症状是 yield 变成 no-op——只有一个线程在跑，另一个永远得不到调度。检查 schedule() 中是否也正确保存了 prev。

**Higher-Half 修了但大内核起不来**: 检查 bootloader 的页表是否正确映射了 higher-half 区域。大内核运行在 0xFFFFFFFF81000000 需要 PML4[511] -> PDPT[510] -> PD -> 2MB pages 这条映射链路完整。如果 bootloader 只建立了 PML4[0] 的恒等映射而没有建立 higher-half 映射，大内核跳转到 higher-half 地址后立刻 page fault。

**AddressSpace 隔离测试失败**: 如果在 AddressSpace A 中映射的页面能从 AddressSpace B 中通过 translate 看到物理地址，说明 PML4 条目还在共享。检查构造函数是否只复制了 PML4[256..511]——PML4[0] 不应该被复制到新的 AddressSpace 中。

**大页拆分后性能下降**: 拆分大页意味着每次内存访问多了一层页表查找。如果你发现拆分后某些操作变慢了，这是正常的——4KB 页的 TLB 覆盖范围比 2MB 页小。后续可以通过 TLB flush 优化或者减少不必要的拆分来缓解。

## 本章小结

这一篇我们修掉了 tag 019 中三个关键 bug。线程退出崩溃是两个 bug 叠加的结果——栈上缺少返回地址加上 exit_current 的指针覆盖，两个修复合力让线程能够干净地退出。Higher-Half ELF 入口地址的修复看起来只是一行代码的改动，但它修复了一个从 bootloader 阶段就存在的根本性问题——大内核运行在错误的虚拟地址上，进程地址空间隔离名存实亡。

这三个 bug 有一个共同特点：它们都不是编译错误或立即 crash 的明显错误。Higher-Half 的 bug 让内核在错误地址空间正常运行了很久，直到 AddressSpace 隔离测试才暴露。线程退出的两个 bug 叠在一起，0xDEADC0DE 的 RIP 看起来像栈溢出（实际上不是），exit_current 的空操作让线程"假装切换了"继续跑。内核调试的经验之一就是：你看到的现象往往不是根因。

到这一步，tag 019 的全部工作已经完成：进程数据结构、上下文切换汇编原语、RoundRobin 调度器、线程创建和退出机制，以及三个关键 bug 的修复。我们的内核从一个单线程的裸机程序进化成了一个能运行多线程的协作式调度系统——接下来的 tag 将在此基础上添加抢占式调度（定时器中断驱动的时间片）、用户态进程切换（CR3 地址空间切换）和系统调用。

## 延伸阅读

- **document/notes/019_proc_context/001_higher_half_fix.md**: Higher-Half 修复的完整调试笔记
- **document/notes/019_proc_context/002_thread_exit_crash.md**: 线程退出崩溃的完整排查笔记
- **Intel SDM Vol.3A Section 4.5.2**: CR3 与地址空间切换
- **Intel SDM Vol.3A Section 4.3.2**: 大页（2MB/1GB）的页表格式
- **PintOS `kernel_thread()` wrapper** (https://uchicago-cs.github.io/mpcs52030/switch.html): 线程退出封装的替代方案参考
- **Linux `switch_to` 宏的 prev 指针谜题** (https://blog.codingconfessions.com/p/linux-context-switching-internals): 类似的指针覆盖问题在 Linux 中的解决方案
