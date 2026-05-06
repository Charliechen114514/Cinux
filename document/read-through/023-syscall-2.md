# 023-2 Read-through: syscall.cpp/hpp + 分发机制与 Handler 实现

## 概览

本文覆盖 syscall 子系统的 C++ 层：`syscall.hpp`（接口定义）、`syscall.cpp`（MSR 配置与分发逻辑）、`syscall_nums.hpp`（调用号常量），以及三个 handler 的实现——`sys_write.cpp`、`sys_exit.cpp`、`sys_yield.cpp`。这些文件构成了从汇编入口到具体系统调用服务之间的软件桥梁。

关键设计决策：统一函数签名（6 参数 int64_t 返回值）、查表分发、用户地址验证、sys_exit 的防御性调度器检查。

## 架构图

```
syscall_entry (syscall.S)
    │
    ▼
syscall_dispatch() (syscall.cpp, extern "C")
    │
    ├─ nr >= 256? → return -1
    ├─ table[nr] == nullptr? → return -1
    │
    ▼
syscall_table[nr](a1..a6)
    │
    ├─ [1] sys_write(fd, buf_virt, count, ...)
    ├─ [24] sys_yield(...)
    ├─ [60] sys_exit(code, ...)
    └─ [other] ... (nullptr → -1)
```

## 代码精讲

### syscall.hpp -- 接口与类型定义

```cpp
using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
```

SyscallFn 是整个分发机制的基石——所有 handler 必须匹配这个签名。六个 uint64_t 参数对应 SYSCALL 约定的 RDI/RSI/RDX/R10/R8/R9，返回值在 RAX。用统一的 6 参数签名而不是每个 handler 各自定义签名，使得分发表只需一种类型。

头文件还声明了三个公开接口：syscall_init(uint64_t kernel_rsp) 配置 MSR 和初始化分发表（接受内核栈指针作为参数），syscall_register() 注册 handler（接受 SyscallNr 枚举和函数指针），syscall_get_kernel_rsp() 读回保存的内核栈指针。以及两个 extern "C" 声明：syscall_entry()（汇编入口，不能被 C++ 编译器 name mangling）和 syscall_dispatch()（被汇编调用，同样需要 C 链接）。

### syscall_nums.hpp -- 系统调用号

```cpp
enum class SyscallNr : uint64_t {
    SYS_read     = 0,
    SYS_write    = 1,
    SYS_open     = 2,
    SYS_close    = 3,
    SYS_stat     = 4,
    SYS_fstat    = 5,
    SYS_chdir    = 12,
    SYS_exit     = 60,
    SYS_yield    = 24,
    SYS_getcwd   = 79,
    SYS_getdents = 78,
    SYS_mkdir    = 83,
    SYS_rmdir    = 84,
    SYS_creat    = 85,
    SYS_unlink   = 87,
    SYS_pipe     = 22,
    SYS_getpid   = 39,
    SYS_getppid  = 110,
    SYS_fork     = 57,
    SYS_execve   = 59,
    SYS_waitpid  = 61,
};
constexpr uint64_t SYSCALL_TABLE_SIZE = 256;
```

调用号参考 Linux x86-64 约定。使用 enum class 而不是宏或常数，这样编译器会做类型检查——你不能意外地把一个随机整数当系统调用号传。分发表大小 256 是一个合理的上限，Linux 早期也用类似的大小。目前已经有 20 个系统调用号定义，覆盖了文件 I/O、进程管理、目录操作等基本功能。

### syscall.cpp -- MSR 配置与初始化

```cpp
void syscall_init(uint64_t kernel_rsp) {
    g_syscall_kernel_rsp = kernel_rsp;

    for (uint64_t i = 0; i < cinux::syscall::SYSCALL_TABLE_SIZE; i++) {
        syscall_table[i] = nullptr;
    }
```

init 函数接受一个 `kernel_rsp` 参数——调用者负责传入正确的内核栈指针。这个值会在调度器做上下文切换时写入 GS 数据页的 gs:0，被 syscall_entry 加载。然后清空分发表，所有槽位设为 nullptr。

```cpp
    constexpr uint32_t MSR_STAR   = 0xC0000081;
    constexpr uint32_t MSR_LSTAR  = 0xC0000082;
    constexpr uint32_t MSR_SFMASK = 0xC0000084;

    uint64_t star_val = (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32)
                      | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 48);
    write_msr(MSR_STAR, star_val);

    uint64_t entry_addr = reinterpret_cast<uint64_t>(syscall_entry);
    write_msr(MSR_LSTAR, entry_addr);

    uint64_t sfmask_val = 0x200;
    write_msr(MSR_SFMASK, sfmask_val);
```

三个 MSR 的配置。STAR 的 [47:32] 和 [63:48] 都设为 0x08（GDT_KERNEL_CODE）——SYSCALL 方向用 [47:32]，SYSRET 方向用 [63:48]。SYSCALL 时 CPU 从 0x08 派生 CS=0x08（内核代码段）和 SS=0x08+8=0x10（内核数据段）。SYSRET 时 CPU 从 0x08 自动派生 CS=0x08+16|3=0x1B（用户 64 位代码段，RPL=3）和 SS=0x08+8|3=0x13（用户数据段，RPL=3）。LSTAR 写入 syscall_entry 的地址。SFMASK 设为 0x200（bit 9 = IF），这样每次 syscall 进入时中断自动关闭，防止在栈切换的脆弱窗口被打断。

### syscall_dispatch -- C 链接的分发器

```cpp
extern "C" int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6) {
    if (nr >= cinux::syscall::SYSCALL_TABLE_SIZE) {
        cinux::lib::kprintf("[DISPATCH] invalid syscall nr=%u\n", static_cast<unsigned>(nr));
        return -1;
    }

    auto fn = cinux::arch::syscall_table[nr];
    if (fn == nullptr) {
        cinux::lib::kprintf("[SYSCALL] unhandled syscall %u\n", static_cast<unsigned>(nr));
        return -1;
    }

    int64_t ret = fn(a1, a2, a3, a4, a5, a6);
    return ret;
}
```

dispatch 逻辑非常直接：边界检查、空指针检查、调用 handler。这个函数被 syscall.S 中的 call syscall_dispatch 调用，所以必须是 extern "C" 链接。两个返回 -1 的路径分别对应无效调用号（会打印 `[DISPATCH] invalid syscall`）和未注册的 handler（会打印 `[SYSCALL] unhandled syscall`）。

### sys_write.cpp -- 带地址验证和 VFS 支持的输出

```cpp
int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    if (buf_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        const auto* buf = reinterpret_cast<const void*>(buf_virt);
        auto        g   = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->write(file->inode, file->offset, buf, count);
        if (result > 0) {
            file->offset += static_cast<uint64_t>(result);
        }
        return result;
    }

    if (fd == 1) {
        const auto* buf = reinterpret_cast<const char*>(buf_virt);
        for (uint64_t i = 0; i < count; i++) {
            kprintf("%c", buf[i]);
        }
        return static_cast<int64_t>(count);
    }

    return -1;
}
```

sys_write 是三个 handler 中最值得细看的。首先做 canonical address 验证——检查 bit 47 的值，如果 bit 47=0 则 bit 48-63 必须全为 0（用户空间地址），如果 bit 47=1 则 bit 48-63 必须全为 1（内核空间地址）。如果用户传入一个非 canonical 地址，这个检查会直接拦截。然后优先检查 fd 是否有 VFS 文件条目（例如管道），如果有就走 VFS 写路径。只有当 fd=1 且没有 VFS 条目时，才走传统的 kprintf 串口输出路径。

说句实话，这里的地址验证是最基本的——它只检查地址是否为 canonical，不检查该地址是否真的被映射到当前进程的地址空间。更完善的实现需要配合页表遍历或 copy_from_user 机制。但对于教学 OS 来说，这个检查足以说明安全验证的重要性。

### sys_exit.cpp -- 防御性进程终止

```cpp
int64_t sys_exit(uint64_t code, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        task->state = cinux::proc::TaskState::Dead;
        cinux::lib::kprintf("[SYSCALL] sys_exit(%u) from tid=%u '%s'\n",
                            static_cast<unsigned>(code), static_cast<unsigned>(task->tid),
                            task->name);
    }
    if (cinux::proc::Scheduler::is_initialized()) {
        cinux::proc::Scheduler::yield();
    } else {
        cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
    return 0;
}
```

sys_exit 的关键设计在于 is_initialized() 检查。在 023 阶段，调度器还没启动——用户程序 exit 后直接 cli; hlt 停机是合理的。到了 024 阶段调度器启动后，exit 就会走 yield 路径，让下一个就绪任务运行。这种设计让 syscall 模块可以在不同的运行环境下都能正确工作，不需要在 init 时传入调度器状态。

### sys_yield.cpp -- 最简 handler

```cpp
int64_t sys_yield(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    cinux::proc::Scheduler::yield();
    return 0;
}
```

sys_yield 就是 yield 的 syscall 封装——没什么额外逻辑。调用 yield 后调度器会选择下一个任务执行，当前任务回到就绪队列。这些 handler 都在 `cinux::syscall` 命名空间中，通过 syscall_register 注册到分发表。

## 设计决策

### 决策：统一 6 参数签名

**问题**：不同 syscall 需要不同数量的参数（sys_exit 只要 1 个，sys_write 要 3 个），分发表如何统一？

**本项目的做法**：所有 handler 统一签名 `int64_t(uint64_t x6)`，多余的参数忽略。

**备选方案**：用变参模板或 std::function 封装。

**为什么不选备选方案**：内核不用标准库，变参模板增加编译复杂度。统一的 C 函数指针类型简单可靠，且和 Linux 的做法一致。性能也是最优的——直接调用没有间接层。

### 决策：sys_exit 的防御性分支

**问题**：023 阶段没有调度器，sys_exit 调用 yield 会崩溃。

**本项目的做法**：运行时检查 Scheduler::is_initialized()，未初始化走 halt 路径。

**备选方案**：要求 023 阶段先启动调度器。

**为什么不选备选方案**：强制要求初始化顺序会增加里程碑间的耦合。防御性设计让每个模块可以独立测试。

## 扩展方向

- ⭐ 添加 sys_getpid handler（返回 current()->tid）
- ⭐⭐ 实现 errno 机制：handler 返回 -1 时在某个 per-thread 位置设置具体的错误码
- ⭐⭐⭐ 用 copy_from_user/copy_to_user 替代 sys_write 中的直接指针访问，配合缺页处理

## 参考资料

- Intel SDM Vol.4: IA32_STAR (0xC0000081), IA32_LSTAR (0xC0000082), IA32_FMASK (0xC0000084) MSR 定义
- Linux kernel syscall table: arch/x86/entry/syscalls/syscall_64.tbl
  - https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl
- xv6 syscall.c: 函数指针分发表 + syscall() 分发
  - https://pdos.csail.mit.edu/6.S081/2024/labs/syscall.html
