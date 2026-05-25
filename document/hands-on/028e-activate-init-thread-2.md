---
title: 028e-activate-init-thread-2 · Init 线程
---

# init 线程实现与用户态启动

> 标签：init thread, ext2 mount, VFS, launch_first_user, placement new, singleton
> 前置：[028e-1 从 kernel_main 到 init 线程](028e-activate-init-thread-1.md)

## 导语

上一章我们把 `kernel_main()` 拆成了「早期初始化 + 调度器启动 + idle 循环」的三段式结构，创建了 init 线程的骨架。现在我们要填充 init 线程的实际内容——ext2 挂载、VFS 注册、以及第一个用户态程序的启动。这一步看起来只是把代码从 `kernel_main()` 搬到 `kernel_init_thread()` 里，但实际上会暴露出两个非常隐蔽的 bug：一个是 `launch_first_user()` 里的 `static Task shell_task{}` 伪造问题，另一个是 `AddressSpace` 析构器导致的 `__dso_handle` 链接错误。

完成本章后，我们的内核就能以一个正规的、调度器管理的 init 内核线程为起点，经过 ext2 挂载和 VFS 注册，最终在 Ring 3 启动 shell。整个链条中没有伪造的 Task，没有绕过调度器的 hack，一切都是干干净净的。

## 概念精讲

### shell_task 伪造问题

在 028d 之前的实现中，`launch_first_user()` 函数里有这么一行：声明一个 `static Task shell_task{}`，然后把它的 cwd 设成 "/"，state 设成 Running，再通过 `Scheduler::set_current(&shell_task)` 把它塞进调度器的当前任务指针。这个 Task 是零初始化的——这意味着它的内核栈指针是 NULL，tid 是 0，CPU 上下文全是零，它根本不在任何运行队列里。

为什么之前没出问题呢？因为之前的启动流程是线性的——shell 启动之后就不会再回到内核态了（至少在早期 tag 里是这样）。但当我们的 shell 开始使用 `sys_exit` 系统调用时，问题就来了：`sys_exit` 会调用 `Scheduler::current()` 拿到这个伪造的 Task，标记它为 Dead，然后 yield。调度器发现当前 Task 被标记为 Dead，就会从运行队列中挑选下一个——但这个伪造的 Task 从来就没在运行队列里，所以 dequeue 操作会在队列中找不到它。如果调度器后来试图切回这个 Task（比如中断返回），它会发现 CPU 上下文全是零，RIP 跳到地址 0x0，直接 triple fault。

真正的修复方案很简单：不用伪造 Task，直接使用调度器已经管理的 init 线程 Task。init 线程是由 `TaskBuilder::build()` 正确创建的——有真实的内核栈、有效的 tid、完整的 CPU 上下文、在运行队列中。我们只需要在 `launch_first_user()` 里获取当前 Task，然后更新它的 `addr_space` 和 `cwd` 字段就行了。

### Placement New 与 `__dso_handle`

`launch_first_user()` 需要创建一个 `AddressSpace` 对象来管理用户态的页表。在 028d 之前的代码中，这个对象是直接在栈上声明的——`AddressSpace user_space;`。这在编译时没有问题，但当 `AddressSpace` 类有了析构器之后，编译器会为栈上的对象注册一个 atexit 回调（通过 `__dso_handle`），用于在作用域结束时调用析构器。问题是我们的内核没有链接标准库，也没有 `__dso_handle` 的定义，所以链接阶段直接报 undefined reference 错误。

解决方案是 placement new——在一个 static 字节数组上手动构造 `AddressSpace` 对象。因为字节数组本身没有析构器，编译器不会注册 atexit 回调。`AddressSpace` 对象虽然在 static 存储上构造了，但我们永远不会显式调用它的析构器——这个对象的生命周期和内核一样长，内核关机时不需要释放它。

### AHCI 单例模式

`init.cpp` 被编译进 `big_kernel_common` 库（一个 OBJECT library），而 AHCI 实例 `ahci` 是 `kernel_main()` 中的局部变量，定义在 `main.cpp` 中（`big_kernel` 可执行文件）。当链接测试二进制时，只链接 `big_kernel_common` 而不链接 `big_kernel`，所以 `init.cpp` 里对 AHCI 的引用就会找不到。

解决方案是给 AHCI 类添加静态的 `instance()` 和 `set_instance()` 方法——一个典型的 Meyer's Singleton 变体（虽然不是线程安全的，但我们的内核初始化是单线程的，所以没关系）。`kernel_main()` 在 AHCI 初始化完成后调用 `set_instance(&ahci)`，init 线程通过 `AHCI::instance()` 获取引用。

## 动手实现

### Step 1: 实现 init 线程的完整流程

**目标**: 在 `kernel/proc/init.cpp` 中实现 init 线程的完整工作流：ext2 挂载、VFS 注册、用户态启动。

**设计思路**: init 线程的执行序列是固定的：首先通过 `Scheduler::current()` 获取自己的 Task 信息并打印日志；然后创建 ext2 实例并挂载——ext2 的构造函数需要 AHCI 引用（通过 `AHCI::instance()` 获取）和端口号；接着初始化 VFS 挂载表，把 ext2 注册到根路径 "/"；最后调用 `launch_first_user()` 启动第一个用户态程序。

ext2 实例需要声明为 `static`——因为它必须在整个内核生命周期中存活。如果声明为普通局部变量，函数返回后它就会被销毁（如果它有析构器的话），而 shell 和其他进程还需要通过 VFS 访问 ext2。

**实现约束**: init 线程函数签名为 `void kernel_init_thread()`，无参数无返回值。包含必要的头文件：AHCI、ext2、VFS、usermode、scheduler、sync。调用 `Scheduler::exit_current()` 结束线程。

**踩坑预警**: 如果你忘了在 `kernel_main()` 中调用 `AHCI::set_instance()`，init 线程里的 `AHCI::instance()` 会返回空指针，然后 ext2 构造函数解引用空指针直接 triple fault。确保 `set_instance()` 在 `Scheduler::init()` 之前被调用。

**验证**: 运行内核，串口输出应显示：
```
[INIT] kernel_init started tid=2
[INIT] ===== Milestone 028: ext2 Filesystem =====
[EXT2] Superblock: magic=0xef53 ...
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[INIT] ===== Milestone 023: Syscall from Ring 3 =====
[USER] Setting up first user-mode program...
```

### Step 2: 修复 launch_first_user 的 Task 问题

**目标**: 移除 `usermode.cpp` 中伪造的 `static Task shell_task{}`，改为使用调度器管理的当前 Task。

**设计思路**: init 线程在调用 `launch_first_user()` 时，`Scheduler::current()` 返回的就是 init 线程的 Task——一个有完整内核栈、有效 tid、在运行队列中的正规 Task。我们只需要更新它的两个字段：`addr_space` 指向新创建的用户态地址空间，`cwd` 设为 "/"。

**实现约束**: 获取当前 Task 后检查是否为空（防御性编程），然后更新 `addr_space` 和 `cwd`，再调用 `set_current()` 通知调度器 Task 已更新。不再需要手动设置 `state`——Task 已经是 Running 状态了。

**踩坑预警**: 千万不要删除 `Scheduler::set_current()` 调用。虽然看起来好像只是更新了一个指针，但调度器内部的 `g_per_cpu.current` 也需要同步更新，否则后续的系统调用会拿到过期的 Task 指针。

**验证**: shell 启动后尝试输入命令，如果 shell 能正常执行 `sys_exit`（比如输入 exit），不应该看到 kernel panic 或者 triple fault。

### Step 3: AddressSpace 的 Placement New

**目标**: 将栈上分配的 `AddressSpace` 改为 placement new 在 static buffer 上构造。

**设计思路**: 声明一个 `alignas(alignof(AddressSpace)) static uint8_t storage[sizeof(AddressSpace)]` 的字节数组作为存储空间，然后用 placement new 在这个数组上构造 AddressSpace 对象。之后所有对 `user_space` 的成员访问都改成指针访问（`user_space->map()` 而不是 `user_space.map()`）。

**实现约束**: 需要包含 `<new>` 头文件来使用 placement new。字节数组的对齐必须等于 `alignof(AddressSpace)`，否则可能是未定义行为。整个文件中所有对 `user_space` 的点号访问都要改成箭头访问。

**验证**: 编译通过，不出现 `__dso_handle` 的 undefined reference 错误。运行时用户态地址空间正常激活。

### Step 4: AHCI 单例封装

**目标**: 在 AHCI 类中添加静态的 `instance()`、`set_instance()` 方法和静态成员指针。

**设计思路**: 在 AHCI 类中声明一个 `static AHCI* s_instance_` 指针，初始值为 nullptr。`set_instance()` 设置这个指针，`instance()` 返回解引用后的引用。在 `.cpp` 文件中定义静态成员并实现这两个方法。

**实现约束**: 静态成员指针需要在 `.cpp` 文件中定义（不能在头文件中 inline 定义，否则多个翻译单元会各自有一份副本）。`instance()` 如果在 `set_instance()` 之前被调用会解引用空指针——但我们不检查这个，因为这是一个内核内部的 API，调用顺序由开发者保证。

**验证**: 编译通过，包括测试二进制（它只链接 `big_kernel_common`）。运行时 init 线程能正确获取 AHCI 实例并完成 ext2 挂载。

## 构建与运行

完整的构建和运行命令：
```
cd build && cmake .. && make -j$(nproc)
./run.sh
```

检查串口输出的关键日志序列：
```
[INIT] kernel_init started tid=2
[INIT] ===== Milestone 028: ext2 Filesystem =====
[EXT2] Superblock: magic=0xef53 rev=1.0 ...
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[INIT] ===== Milestone 023: Syscall from Ring 3 =====
[USER] Setting up first user-mode program...
[USER] User address space activated
[USER] Jumping to Ring 3
```

## 调试技巧

**init 线程中 ext2 mount 失败**：如果看到 `[INIT] ext2 mount failed!`，首先检查 AHCI Port 1 是否检测到设备（在 AHCI init 阶段应该看到 `SSTS=0x113 DET=3`）。如果 init 线程中 Port 1 的 SSTS 变成了 0x0，那说明 MMIO 映射被覆盖了——这是下一章要讨论的核心问题。

**`__dso_handle` 链接错误**：如果链接阶段报 `undefined reference to '__dso_handle'`，说明某个有析构器的对象被声明为栈变量或全局变量。检查 `launch_first_user()` 中的 `AddressSpace` 是否改成了 placement new。

**shell 启动后立刻崩溃**：如果 shell 的 Ring 3 入口执行后立刻触发异常，检查 `current->addr_space` 是否正确设置了——如果 `launch_first_user()` 里的 `Scheduler::current()` 返回了 nullptr，addr_space 就不会被设置，用户态页表就没有被正确激活。

## 本章小结

| 概念 | 说明 |
|------|------|
| shell_task 伪造 | 零初始化的假 Task，没有内核栈，不在运行队列 |
| 调度器管理的 Task | 由 TaskBuilder 正确创建的 Task，有完整生命周期 |
| Placement new | 在预分配的 static buffer 上构造对象，避免析构器链接问题 |
| AHCI 单例 | `instance()/set_instance()` 解决跨翻译单元访问 |
| `Scheduler::exit_current()` | init 线程完成后的正常退出方式 |

到这里，init 线程的核心实现就完成了。但在实际测试中，你大概率会遇到一个非常诡异的问题——ext2 挂载在 AHCI init 阶段显示设备正常，但 init 线程中去读就超时了。这就是我们下一章要拆解的 MMIO 地址冲突问题，说实话这个 bug 真的坑了我半天。
