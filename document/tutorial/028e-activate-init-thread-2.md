---
title: 028e-activate-init-thread-2 · Init 线程
---

# 实现 kernel_init_thread——ext2 挂载与 shell 启动

> 标签：init thread, ext2 mount, VFS, launch_first_user, shell_task, AHCI singleton
> 前置：[028e-1 为什么要引入 init 线程](028e-activate-init-thread-1.md)

## 前言

上一篇我们搭好了 init 线程的架子——`kernel_main()` 启动调度器，创建 init 线程和 boot task，然后 `run_first()` 把执行流交给了 `kernel_init_thread()`。现在 init 线程需要做三件事：挂载 ext2 文件系统、注册 VFS 挂载表、启动第一个用户态 shell。听起来就是把原来 `kernel_main()` 里的代码搬个家，对吧？差不多是这样，但搬家的过程中暴露出了两个非常隐蔽的 bug，一个在 `launch_first_user()` 里，一个在跨翻译单元的对象访问上。

说实话，shell_task 那个 bug 是我们在 028d 就埋下的定时炸弹。当时 `launch_first_user()` 为了让 shell 有一个「看起来像真的」Task 上下文，手动创建了一个零初始化的 static Task，设置了 cwd 和 state，然后通过 `set_current()` 塞给调度器。这个 hack 在 shell 不退出的时候能凑合，但在 init 线程模型下就彻底暴露了——init 线程才是真正的 Task 上下文，根本不需要伪造任何东西。

## 环境说明

本篇涉及两个主要文件的修改：新建 `kernel/proc/init.cpp`（init 线程实现）和修改 `kernel/arch/x86_64/usermode.cpp`（shell_task 修复 + placement new）。还涉及 `kernel/drivers/ahci/ahci.hpp/cpp` 的单例封装。工具链和环境不变：GCC 14 + CMake + QEMU，内核运行在 higher-half。

## 第一步——实现 init 线程的完整流程

init 线程的实现非常直接——它把原来 `kernel_main()` 中的 ext2 挂载和 VFS 注册代码原封不动地搬过来，只改了 AHCI 实例的获取方式：

```cpp
void kernel_init_thread() {
    auto* self = Scheduler::current();
    cinux::lib::kprintf("[INIT] kernel_init started tid=%u\n",
                        self ? self->tid : 0);
```

第一行获取当前 Task。因为 init 线程是在调度器管理下运行的，`current()` 一定返回有效的 Task。打印 tid 是一个启动确认信号。

```cpp
    static cinux::fs::Ext2 ext2(cinux::drivers::ahci::AHCI::instance(), 1);
    if (!ext2.mount()) {
        cinux::lib::kprintf("[INIT] ext2 mount failed!\n");
    }
```

`static` 关键字保证 ext2 实例的生命周期和内核一致——如果省略 static，函数返回后 ext2 就被销毁了，而 shell 还需要通过 VFS 访问它。`AHCI::instance()` 返回在 `kernel_main()` 中注册的 AHCI 实例引用，端口号 `1` 是 QEMU 中 SATA 硬盘的默认端口。这里和原来在 `kernel_main()` 中的唯一区别就是 AHCI 引用的获取方式——从局部变量变成了单例。

VFS 注册部分和原来一模一样，搬过来就行：

```cpp
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", &ext2);
```

然后是用户态启动：

```cpp
    cinux::arch::launch_first_user();

    cinux::lib::kprintf("[INIT] launch_first_user returned, exiting.\n");
    Scheduler::exit_current();
```

`launch_first_user()` 通常不会返回（它通过 `jump_to_usermode()` 跳转到 Ring 3）。但如果 shell 退出了（比如用户输入 exit），init 线程会走到 `exit_current()`，让调度器安全地清理这个 Task。这和 Linux 中 `kernel_init` 最后调用 `do_exit()` 是同一个模式。

## 第二步——修复 launch_first_user 的 shell_task 伪造

现在我们来看 `usermode.cpp` 中的修改——这是本篇最关键的部分。

原来的代码是这样的：

```cpp
// 原来的 launch_first_user 尾部
static cinux::proc::Task shell_task{};
shell_task.cwd[0] = '/';
shell_task.cwd[1] = '\0';
shell_task.state = cinux::proc::TaskState::Running;
cinux::proc::Scheduler::set_current(&shell_task);

jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

这个 `static Task shell_task{}` 是一个零初始化的结构体——内核栈指针是 NULL、tid 是 0、CPU 上下文全是零、不在任何运行队列中。它就像一张伪造的身份证——平时看不出来，但当 `sys_exit` 尝试把它标记为 Dead 并从运行队列中移除时，dequeue 操作找不到它；当调度器尝试切回它时，RIP 跳到 0x0，直接 triple fault。

修复后的代码：

```cpp
auto* current = cinux::proc::Scheduler::current();
if (current != nullptr) {
    current->addr_space = user_space;
    current->cwd[0] = '/';
    current->cwd[1] = '\0';
    cinux::proc::Scheduler::set_current(current);
}
```

现在 `Scheduler::current()` 返回的是 init 线程的 Task——一个由 `TaskBuilder::build()` 正确创建的 Task，有真实的内核栈、有效的 tid、完整的 CPU 上下文、在运行队列中。我们只需要更新两个字段：`addr_space` 指向新创建的用户态地址空间，`cwd` 设为根目录 "/"。

你会发现，修复之后代码反而更简洁了——不再需要手动设置 state（Task 已经是 Running），不需要创建假 Task，也不需要担心 `sys_exit` 路径的崩溃问题。这才是操作系统该有的样子——每个在运行的代码都有一个有效的 Task 上下文，没有例外。

## 第三步——AddressSpace 的 placement new

`launch_first_user()` 中还有一个隐蔽的链接问题。原来的代码在栈上创建 AddressSpace：

```cpp
// 原来的代码
AddressSpace user_space;
```

这在编译时没问题，但当 `AddressSpace` 类有了析构器之后（它会释放页表占用的物理页），编译器会注册一个 atexit 回调来在作用域结束时调用析构器。这个回调通过 `__cxa_atexit(__dso_handle, ...)` 实现——但我们的内核没有标准库，也没有 `__dso_handle` 的定义，所以链接阶段直接报 undefined reference。

修复方案是 placement new——在一个无析构器的 static 字节数组上手动构造对象：

```cpp
alignas(alignof(AddressSpace)) static uint8_t user_space_storage[sizeof(AddressSpace)];
auto* user_space = new (user_space_storage) AddressSpace;
```

`alignas` 保证存储空间的对齐和 `AddressSpace` 一致（否则是未定义行为）。`placement new` 在这个数组上调用 `AddressSpace` 的构造函数，返回一个指针。因为 `uint8_t` 数组没有析构器，编译器不会注册 atexit 回调——问题解决。

之后所有对 `user_space` 的访问从点号改成了箭头（`user_space->map()` 而不是 `user_space.map()`），因为现在它是指针而不是对象了。

## 第四步——AHCI 单例封装

最后一个需要解决的跨翻译单元访问问题。`init.cpp` 被编译进 `big_kernel_common`（一个 OBJECT library），而 AHCI 实例是 `kernel_main()` 中的局部变量，定义在 `main.cpp`（属于 `big_kernel` 可执行文件）。当链接测试二进制时，只链接 `big_kernel_common` 而不链接 `big_kernel`，所以 `init.cpp` 里对 AHCI 的引用会找不到定义。

解决方案是标准的 Singleton 模式：

```cpp
// ahci.hpp
class AHCI {
public:
    static AHCI& instance();
    static void  set_instance(AHCI* ahci);
    // ...
private:
    static AHCI* s_instance_;
};

// ahci.cpp
AHCI* AHCI::s_instance_ = nullptr;
AHCI& AHCI::instance() { return *s_instance_; }
void AHCI::set_instance(AHCI* ahci) { s_instance_ = ahci; }
```

`kernel_main()` 在 AHCI 初始化后调用 `set_instance(&ahci)`，init 线程通过 `AHCI::instance()` 获取引用。这不是线程安全的（没有 double-checked locking），但在我们的单核内核中不存在并发问题。

## 与其他 OS 的对比

### Linux 的 kernel_init 实现

Linux 的 `kernel_init` 比 Cinux 的复杂得多。它首先调用 `kernel_init_freeable()` 完成剩余的驱动初始化（通过 `do_initcalls()` 遍历所有 `__init` 段的函数），然后尝试 exec 一系列 init 程序。如果 `/sbin/init`、`/etc/init`、`/bin/init` 都失败了，最后尝试 `/bin/sh`。如果连 shell 都启动不了，kernel panic。

Cinux 的 init 线程不需要 initcall 基础设施（我们的驱动数量不多，直接在函数中调用就行），也没有 exec 的 fallback 列表（只有一个 `launch_first_user()` 跳到内嵌的 shell 二进制）。但设计思想完全一致：内核线程负责 late init，然后过渡到用户态。

### SerenityOS 的 Process::create_kernel_process

SerenityOS 使用 `Process::create_kernel_process("init", ...)` 来创建 init 进程。和 Cinux 类似，init 进程在内核态完成文件系统挂载后，通过 fork + exec 启动用户态的 `/bin/SystemServer`。SerenityOS 的实现更接近现代 Unix——init 程序是一个真正的用户态可执行文件，而不是从内核直接跳转。

## 收尾

到这里，init 线程的核心实现就完成了。现在构建并运行内核，你应该能看到完整的启动序列：

```
[BIG] ===== Scheduler & Init Thread =====
[SCHED] Idle task created tid=1
[PROC] Created task tid=2 name='kernel_init' stack=0x...
[INIT] kernel_init started tid=2
[INIT] ===== Milestone 028: ext2 Filesystem =====
[EXT2] Superblock: magic=0xef53 ...
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[INIT] ===== Milestone 023: Syscall from Ring 3 =====
[USER] Setting up first user-mode program...
[USER] Jumping to Ring 3
shell> _
```

不过，如果你真的跑了这一版代码，大概率会遇到一个诡异的问题——ext2 挂载会报 "command timeout"，shell 启动不了。别慌，这就是我们下一篇要拆解的 MMIO 地址冲突——一个让我血压拉满的 bug。

## 参考资料

- Linux init/main.c: `kernel_init()` — [GitHub](https://github.com/torvalds/linux/blob/master/init/main.c)
- xv6 proc.c: `userinit()` — [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c)
- C++ Placement New: [cppreference](https://en.cppreference.com/w/cpp/language/new#Placement_new)
- OSDev Wiki: [C++](https://wiki.osdev.org/C%2B%2B) — 内核中使用 C++ 的注意事项（析构器、new/delete 等）
