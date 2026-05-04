# 031-3: 系统集成 — sys_read/write 重构与 GUI Shell 启动

## 概览

本文是 tag 031 通读系列的第三篇，聚焦于将 pipe 和 terminal 两个子系统集成为完整可用的 GUI shell 终端。涉及的改动分布在多个文件中：sys_read/sys_write 的路由重构、FDTable 的 set 扩展、gui_init.cpp 中的 create_shell_terminal 启动流程、以及 window_manager 和 PIT tick 回调的终端轮询支持。

这些改动虽然分散，但它们共同解决了一个问题：让 shell 进程的标准 I/O 不再绑定到串口，而是通过管道连接到 GUI 终端窗口。

## 架构图

```
gui_init.cpp: create_shell_terminal()
         |
         v
    +-----------+     fork()     +------------------+
    | gui_worker | -----------> | shell child proc |
    | (kernel)   |              |  (user mode)     |
    +-----------+              +------------------+
         |                              |
    Terminal window                FDTable (private)
    stdin_pipe (try_write)         fd[0] -> PipeReadOps -> stdin_pipe (read)
    stdout_pipe (try_read)         fd[1] -> PipeWriteOps -> stdout_pipe (write)
         |                              |
    on_key -> stdin_pipe          sys_read(0) -> VFS -> pipe
    poll_output <- stdout_pipe    sys_write(1) -> VFS -> pipe
         |
    render_to_canvas -> Canvas
         |
    WindowManager::composite -> Screen
```

## 代码精讲

### FDTable::set 扩展 (fs/file.cpp)

```cpp
bool FDTable::set(int fd, File* file) {
    auto g = lock_.guard();
    (void)g;
    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) return false;
    fds_[fd] = file;
    return true;
}
```

之前 FDTable 只有 alloc（从 fd=3 开始搜索空闲槽）和 close，无法在指定位置安装文件描述符。但 shell 终端初始化需要把管道的读端安装到 fd 0、写端安装到 fd 1——这是 POSIX 标准的 stdin/stdout 编号。set 方法就是为这个需求而生的：直接在指定槽位放入 File 指针，不做任何搜索。

注意 set 不负责释放之前槽位上的 File——调用方需要在调用 set 之前手动处理。在 Cinux 的使用场景中，set 只在 shell 子进程的 FDTable（刚创建，所有槽位都是 nullptr）上调用，所以不存在覆盖问题。

### sys_read 路由重构 (syscall/sys_read.cpp)

```cpp
int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    // Address validation ...
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        auto* buf = reinterpret_cast<void*>(buf_virt);
        auto  g   = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->read(file->inode, file->offset, buf, count);
        if (result > 0) file->offset += static_cast<uint64_t>(result);
        return result;
    }

    // Legacy fd=0 keyboard path ...
    if (fd == 0) { /* poll keyboard buffer */ }
    return -1;
}
```

重构的核心是增加了一个"VFS 优先"的路由层。在处理具体的 fd 编号之前，先通过 FDTable 查找是否有对应的 VFS 条目。三个条件必须同时满足：file 非 null（fd 已分配）、inode 非 null（有关联的文件）、ops 非 null（有操作实现）。如果都满足，就走 InodeOps->read 路径，完全由 inode 的操作函数决定行为。

这个设计的精妙之处在于对用户态完全透明。Shell 进程调用 `sys_read(0, buf, 256)` 时，它不知道 fd 0 背后是管道还是键盘缓冲区——路由由内核根据 FDTable 的内容自动决定。

### sys_write 路由重构 (syscall/sys_write.cpp)

```cpp
int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    // Address validation ...
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        const auto* buf = reinterpret_cast<const void*>(buf_virt);
        auto  g   = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->write(file->inode, file->offset, buf, count);
        if (result > 0) file->offset += static_cast<uint64_t>(result);
        return result;
    }

    // Legacy fd=1 kprintf path ...
    if (fd == 1) { /* kprintf to serial */ }
    return -1;
}
```

sys_write 的重构模式完全一样。当 shell 进程调用 `sys_write(1, buf, n)` 时，VFS 路径找到 fd 1 对应的 PipeWriteOps，调用其 write 方法，数据通过管道流向终端的 poll_output。

对于没有通过 sys_pipe 设置管道的进程（比如内核线程），fd 0 和 fd 1 在 FDTable 中没有条目，VFS 路径不命中，自然回退到传统的键盘缓冲区读取和 kprintf 串口输出。

### create_shell_terminal 启动流程 (gui/gui_init.cpp)

这是整个 tag 中最复杂的函数，负责创建终端窗口、设置管道、fork 子进程、execve shell。

```cpp
void create_shell_terminal() {
    auto& wm = WindowManager::instance();
    g_terminal_counter++;
    char title_buf[64];
    strcpy(title_buf, "Shell #");
    utoa(title_buf + strlen(title_buf), g_terminal_counter);

    uint32_t term_w = Terminal::COLS * 8;   // 640
    uint32_t term_h = Terminal::ROWS * 16;  // 400
    uint32_t term_x = 80, term_y = 60;
    // ... center calculation ...

    auto* term = new Terminal(term_x, term_y, title_buf);
    term->set_font(g_font);
```

首先是准备工作：生成唯一的终端标题（Shell #1, Shell #2...），计算窗口位置（尽量居中），创建 Terminal 对象并设置字体。

```cpp
    // stdin pipe: Terminal.on_key() writes -> shell reads from fd 0
    auto* stdin_pipe       = new cinux::ipc::Pipe();
    auto* stdin_read_ops   = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops  = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    // stdout pipe: shell writes to fd 1 -> Terminal.poll_output() reads
    auto* stdout_pipe        = new cinux::ipc::Pipe();
    auto* stdout_write_ops   = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops  = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);
```

管道设置部分需要仔细理解方向。stdin 管道的读端（PipeReadOps）给了 shell 的 fd 0，写端直接通过 pipe 指针给 Terminal（on_key 时 try_write）。stdout 管道的写端（PipeWriteOps）给了 shell 的 fd 1，读端直接通过 pipe 指针给 Terminal（poll_output 时 try_read）。

```cpp
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid > 0) {
        // Parent path
        term->set_shell_pid(child_pid);
    } else if (child_pid == 0) {
        // Child path
        __asm__ volatile("cli");
        auto* task       = cinux::proc::Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();
        task->fd_table   = new cinux::fs::FDTable();
        task->fd_table->set(0,
            new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY));
        task->fd_table->set(1,
            new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY));

        const char* path   = "/bin/sh";
        const char* argv[] = {path, nullptr};
        const char* envp[] = {nullptr};
        auto result = cinux::proc::execve(path, argv, envp);
```

子进程路径中有几个关键操作。首先是 cli 关中断——fork 后子进程继承了父进程的中断状态，但在设置新的地址空间之前需要关中断防止 PIT 在不完整的 CR3 上触发。然后创建新的 AddressSpace（父进程是内核线程没有地址空间）和独立的 FDTable。最后用 set 把管道 inode 安装到 fd 0 和 fd 1。

```cpp
        // After execve succeeds:
        uint64_t stack_base =
            cinux::arch::USER_STACK_TOP - cinux::arch::USER_STACK_PAGES * cinux::arch::PAGE_SIZE;
        for (uint64_t i = 0; i < cinux::arch::USER_STACK_PAGES; i++) {
            uint64_t phys = cinux::mm::g_pmm.alloc_page();
            uint64_t virt = stack_base + i * cinux::arch::PAGE_SIZE;
            task->addr_space->map(virt, phys, kUserPageFlags);
        }
        task->addr_space->activate();
        cinux::proc::g_per_cpu.update_syscall_stack(task->kernel_stack_top);
        jump_to_usermode(entry, cinux::arch::USER_STACK_TOP - cinux::arch::USER_ABI_RSP_OFFSET, 0);
```

execve 之后需要手动映射用户栈和跳转到用户态。activate() 切换 CR3 到子进程的页表。update_syscall_stack 确保后续 syscall 入口使用子进程的内核栈。最后 jump_to_usermode 通过 SYSRETQ 进入 Ring 3——注意在调用前没有 sti，因为 SYSRETQ 会从 R11 恢复 RFLAGS（包含 IF=1），自动启用中断。

```cpp
    wm.add_window(term);
}
```

父进程路径很简单：记录 shell PID，把终端窗口加入窗口管理器。

### PIT tick 回调中的终端轮询 (gui/gui_init.cpp)

```cpp
void gui_tick_callback(void* /*ctx*/) {
    // ... event processing ...

    // Poll all terminal windows for shell output
    for (uint32_t i = 0; i < wm.window_count(); i++) {
        auto* win = wm.window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<Terminal*>(win);
            term->poll_output();
            term->render_to_canvas();
        }
    }
    wm.composite();
}
```

tick 回调在每次 PIT 中断时执行（约 100Hz）。新增的终端轮询逻辑遍历所有窗口，对 Terminal 类型的窗口执行 poll_output（从 stdout 管道读取 shell 输出）和 render_to_canvas（渲染到 Canvas），最后 composite 把所有窗口合成到屏幕。

这里轮询的是所有终端窗口而不只是焦点窗口——这是支持多 shell 并发的关键。每个 Terminal 有自己独立的管道对，互不干扰。

### 窗口管理器的 Terminal 支持 (gui/window_manager.cpp)

WindowManager 的改动主要是处理桌面图标点击事件，触发 create_shell_terminal 的延迟调用。点击事件不能在 ISR 上下文中直接创建终端（fork 和内存分配不适合在中断中做），所以通过一个 Atomic<IconAction> 的延迟工作队列传递：ISR 中设置 action，gui_worker 线程在主循环中通过 gui_process_pending 检查并执行。

## 设计决策

### 决策：VFS 优先的系统调用路由
**问题**: sys_read/sys_write 怎么同时支持管道和传统设备？
**本项目的做法**: 先查 FDTable 的 VFS 条目，有则走 InodeOps，否则回退硬编码路径。
**备选方案**: 为管道 fd 增加专门的分支（如检查 fd 编号或文件类型）。
**为什么不选备选方案**: VFS 优先路由是更通用的设计——未来加入 socket、设备文件等也不需要改系统调用代码。InodeOps 多态已经提供了正确的分发机制。
**如果要扩展/改进**: 增加更多文件类型（socket、block device）时不需要修改 sys_read/sys_write，只需在 FDTable 中安装对应的 InodeOps。

### 决策：延迟终端创建
**问题**: 桌面图标点击时直接创建终端还是延迟创建？
**本项目的做法**: ISR 中设置 Atomic flag，gui_worker 线程中执行实际创建。
**备选方案**: 直接在 ISR 中 fork + execve。
**为什么不选备选方案**: fork 涉及进程表操作、地址空间拷贝、内存分配，这些在中断上下文中执行不安全（可能嵌套中断、持锁状态复杂）。延迟到线程上下文更安全。
**如果要扩展/改进**: 使用通用的工作队列（work queue）替代单一的 Atomic flag，支持多种延迟任务。

### 决策：SYSRETQ 前不调用 sti
**问题**: 子进程跳转到用户态之前要不要手动启用中断？
**本项目的做法**: 不调用 sti。SYSRETQ 自动从 R11 恢复 RFLAGS（IF=1）。
**备选方案**: 在 jump_to_usermode 前调用 sti。
**为什么不选备选方案**: sti 后、SYSRETQ 前有一个窗口期，此时 CR3 已经是子进程的页表，但 identity mapping 可能不完整。如果 PIT 在这个窗口期触发，ISR 试图访问的内存地址可能在子进程页表中没有映射，直接 page fault。
**如果要扩展/改进**: 如果需要更早启用中断，确保子进程的 AddressSpace 包含必要的 identity mapping（kernel map 区域）。

## 扩展方向

- **多窗口 shell**: 支持同时运行多个终端窗口，每个独立 shell 会话 (难度: 低，已支持)
- **窗口间管道**: 拖拽连接两个终端的输入输出 (难度: 较高)
- **信号传递**: 支持 SIGPIPE（写端已关闭时通知 shell）和 SIGHUP（终端关闭时通知 shell） (难度: 中等)
- **伪终端 (pty)**: 实现 ptmx/pts 设备，支持 SSH 等远程终端 (难度: 较高)
- **dup2 系统调用**: 支持文件描述符重定向，让 shell 能实现管道操作符 (难度: 中等)

## 参考资料

- OSDev Wiki: [Unix Pipes](https://wiki.osdev.org/Unix_Pipes) — 管道的基本概念和 fork/execve/pipe 三件套
- xv6 RISC-V: [pipe.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c) — 经典教学 OS 的管道和 fork/execve 集成
- Intel SDM: Vol.2A SYSRETQ 指令 — R11 恢复 RFLAGS 的行为，IF 位自动设置
