# init.cpp 与 usermode.cpp——init 线程核心实现

> 标签：init thread, ext2 mount, VFS, launch_first_user, placement new, AHCI singleton
> 前置：[028e-1 kernel_main 重构与调度器启动](028e-activate-init-thread-1.md)

## 概览

本文聚焦于 028e 的两个核心实现文件。`kernel/proc/init.cpp` 是新建的 init 线程，负责 ext2 挂载、VFS 注册和用户态启动——它是 `kernel_main()` 中被移除的那部分代码的新家。`kernel/arch/x86_64/usermode.cpp` 经历了关键修改：删除了伪造的 `shell_task`，改用调度器管理的当前 Task，并用 placement new 解决了 `AddressSpace` 的链接问题。

关键设计决策一览：
- init 线程通过 `AHCI::instance()` 单例获取 AHCI 驱动实例
- `static Ext2 ext2(...)` 保证文件系统实例生命周期和内核一致
- `launch_first_user()` 不再创建伪 Task，而是更新当前 Task 的 addr_space 和 cwd
- AddressSpace 使用 placement new 构造在 static buffer 上，避免析构器链接问题

## 架构图

```
kernel_init_thread()
  │
  ├── Scheduler::current() → 获取自身 Task
  ├── AHCI::instance() → 获取 AHCI 驱动引用
  │
  ├── Ext2 ext2(AHCI::instance(), port=1)
  │     └── ext2.mount()
  │
  ├── vfs_mount_init()
  ├── vfs_mount_add("/", &ext2)
  │
  └── launch_first_user()           ← usermode.cpp
        │
        ├── placement new AddressSpace
        ├── 映射用户代码页
        ├── 映射用户栈页
        ├── 拷贝内核 PML4 高半部分
        ├── user_space->activate()
        │
        ├── Scheduler::current() → 获取 init Task
        ├── 更新 addr_space 和 cwd
        │
        └── jump_to_usermode()

  └── Scheduler::exit_current()
```

## 代码精讲

### init.cpp——完整实现

`kernel/proc/init.cpp` 是这个 tag 最核心的新文件，只有 36 行代码（diff 中的版本），但它承担了从「内核初始化完成」到「用户态 shell 运行」之间的全部桥梁工作。

首先是头文件引用：

```cpp
#include "kernel/proc/init.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/per_cpu.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
```

注意 init.cpp 需要包含很多子系统头文件——因为它是「后期初始化」的入口，需要直接操作 AHCI、ext2、VFS、用户态等模块。这也印证了为什么这段代码不应该继续留在 `kernel_main()` 中——依赖太多，会让 `main.cpp` 膨胀得不可维护。

接下来是 init 线程的主体：

```cpp
void kernel_init_thread() {
    auto* self = Scheduler::current();
    cinux::lib::kprintf("[INIT] kernel_init started tid=%u\n",
                        self ? self->tid : 0);
```

`Scheduler::current()` 返回当前正在执行的 Task。init 线程是由 `TaskBuilder::build()` 创建、由 `Scheduler::add_task()` 加入运行队列、由 `run_first()` 调度到的，所以 `current()` 一定能返回一个有效的 Task。打印 tid 是一个很好的启动确认——如果你在串口输出中看到了 `[INIT] kernel_init started tid=2`，说明 init 线程已经被正确调度到了。

然后是 ext2 挂载：

```cpp
    cinux::lib::kprintf("[INIT] ===== Milestone 028: ext2 Filesystem =====\n");
    static cinux::fs::Ext2 ext2(cinux::drivers::ahci::AHCI::instance(), 1);
    if (!ext2.mount()) {
        cinux::lib::kprintf("[INIT] ext2 mount failed!\n");
    }
```

`static` 关键字非常重要——ext2 实例必须在整个内核生命周期中存活。如果省略 `static`，ext2 就是一个普通的局部变量，`kernel_init_thread()` 返回后它就被销毁了，而 shell 和其他进程还需要通过 VFS 访问 ext2。`AHCI::instance()` 返回在 `kernel_main()` 中通过 `set_instance()` 注册的 AHCI 实例引用。端口号 `1` 是 QEMU 中 SATA 硬盘通常连接的端口。

接下来是 VFS 注册：

```cpp
    cinux::lib::kprintf("[INIT] ===== Milestone 027: VFS =====\n");
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", &ext2);
    cinux::lib::kprintf("[VFS] ext2 mounted at /\n");
```

这两行和原来在 `kernel_main()` 中完全一样——初始化 VFS 挂载表，然后把 ext2 注册到根路径。搬到这里只是位置变化，逻辑不变。

最后是用户态启动：

```cpp
    cinux::lib::kprintf("[INIT] ===== Milestone 023: Syscall from Ring 3 =====\n");
    cinux::arch::launch_first_user();

    cinux::lib::kprintf("[INIT] launch_first_user returned, exiting.\n");
    Scheduler::exit_current();
```

`launch_first_user()` 通常不会返回——它通过 `jump_to_usermode()` 永久跳转到 Ring 3。如果它真的返回了（比如 shell 进程通过某种路径退出了），init 线程会调用 `Scheduler::exit_current()` 正常退出。这和 Linux 的 `kernel_init` 末尾调用 `do_exit()` 是同样的模式。

### usermode.cpp——shell_task 修复与 placement new

`usermode.cpp` 中 `launch_first_user()` 函数的修改是这个 tag 中最关键的 bugfix 之一。

首先是 AddressSpace 的创建方式从栈变量改成了 placement new：

```cpp
// 原来：
// AddressSpace user_space;

// 现在：
alignas(alignof(AddressSpace)) static uint8_t user_space_storage[sizeof(AddressSpace)];
auto* user_space = new (user_space_storage) AddressSpace;
```

为什么要这么折腾？因为 `AddressSpace` 有析构器——它会释放页表占用的物理页。当编译器看到栈上有一个带析构器的对象时，它会注册一个 atexit 回调（通过 `__cxa_atexit` 和 `__dso_handle`）。但我们的内核没有标准库，也没有 `__dso_handle` 的定义，链接阶段直接报错。placement new 在一个无析构器的 `uint8_t` 数组上构造对象，绕过了这个问题——编译器不知道数组上有一个需要析构的对象，所以不会注册 atexit 回调。

之后所有对 `user_space` 的访问都从点号改成了箭头——因为现在它是指针而不是对象了：

```cpp
// 原来: user_space.map(virt, code_phys, kUserPageFlags)
// 现在: user_space->map(virt, code_phys, kUserPageFlags)

// 原来: user_space.activate()
// 现在: user_space->activate()

// 原来: user_space.pml4_phys()
// 现在: user_space->pml4_phys()
```

然后是最关键的 Task 修复——移除伪造的 shell_task：

```cpp
// 原来：
// static cinux::proc::Task shell_task{};
// shell_task.cwd[0] = '/';
// shell_task.cwd[1] = '\0';
// shell_task.state = cinux::proc::TaskState::Running;
// cinux::proc::Scheduler::set_current(&shell_task);

// 现在：
auto* current = cinux::proc::Scheduler::current();
if (current != nullptr) {
    current->addr_space = user_space;
    current->cwd[0] = '/';
    current->cwd[1] = '\0';
    cinux::proc::Scheduler::set_current(current);
}
```

原来的代码手动创建了一个零初始化的 `static Task`——没有内核栈、tid 是 0、不在任何运行队列中。现在改为获取调度器已经管理的当前 Task（即 init 线程的 Task），只更新它的 `addr_space`（指向刚创建的用户态地址空间）和 `cwd`（设为根目录）。`set_current()` 同步更新调度器的内部指针和 `g_per_cpu.current`。

这里的 `if (current != nullptr)` 是防御性编程——理论上 current 不可能为空（因为 init 线程是在调度器的管理下运行的），但多一层检查总是好的。

### AHCI 单例实现

在 `ahci.hpp` 中新增了两个静态方法声明和一个静态成员指针：

```cpp
class AHCI {
public:
    static AHCI& instance();
    static void  set_instance(AHCI* ahci);
    // ...
private:
    static AHCI* s_instance_;
    // ...
};
```

在 `ahci.cpp` 中实现了这三个：

```cpp
AHCI* AHCI::s_instance_ = nullptr;

AHCI& AHCI::instance() {
    return *s_instance_;
}

void AHCI::set_instance(AHCI* ahci) {
    s_instance_ = ahci;
}
```

这是最简单的 Singleton 实现——一个裸指针，没有线程安全检查，没有 double-checked locking。对于我们的内核来说这就够了——`set_instance()` 在 `kernel_main()` 中被调用，此时调度器还没启动，只有 boot CPU 在运行；`instance()` 在 init 线程中被调用，此时 `set_instance()` 早已完成。没有任何并发问题。

## 设计决策

### 决策：placement new vs 禁用析构器

**问题**: 如何避免 `AddressSpace` 的析构器导致 `__dso_handle` 链接错误？

**本项目的做法**: 使用 placement new 在 static 字节数组上构造 AddressSpace，永远不调用析构器。

**备选方案**: 提供 `__dso_handle` 的桩实现，或者给 AddressSpace 添加一个不执行任何操作的析构器标记。

**为什么不选备选方案**: 提供桩实现是掩耳盗铃——如果将来其他类也有析构器，问题会反复出现。修改 AddressSpace 的析构行为会影响正常对象的销毁逻辑。placement new 是内核开发中的标准做法——很多内核都这样管理不需要析构的长期存活对象。

### 决策：单例模式 vs extern 全局变量

**问题**: `init.cpp` 如何访问 `main.cpp` 中的 AHCI 实例？

**本项目的做法**: 在 AHCI 类中添加 `static instance()/set_instance()` 方法。

**备选方案**: 在头文件中声明 `extern AHCI g_kernel_ahci;`，在 main.cpp 中定义。

**为什么不选备选方案**: extern 全局变量是 C 风格的做法，不符合现代 C++ 的封装原则。单例模式把实例管理内聚在类中，调用方不需要知道实例存储在哪里。更重要的是，extern 方案在测试二进制中仍然会有链接问题——测试只链接 `big_kernel_common`，不链接 `big_kernel`，所以 `g_kernel_ahci` 的定义找不到。

## 扩展方向

- 给 init 线程添加错误恢复逻辑：如果 ext2 挂载失败，尝试挂载 ramdisk 作为 fallback
- 实现 `fork()` + `execve("/bin/sh")` 模式替代直接 `launch_first_user()`，更接近 Unix init
- 给 AHCI 单例添加 assert 检查：`instance()` 被调用时如果 `s_instance_` 为空就打印错误信息
- 在 init 线程中启动更多系统服务：比如一个日志守护线程、一个网络初始化线程

## 参考资料

- Linux init/main.c: `kernel_init()` — [GitHub](https://github.com/torvalds/linux/blob/master/init/main.c)
- C++ Placement New: [cppreference](https://en.cppreference.com/w/cpp/language/new#Placement_new)
- OSDev Wiki: [Kernel Object Lifecycle](https://wiki.osdev.org/Object_Lifecycle) — 内核中对象析构的处理
