---
title: 035-multi-terminal-3 · 多终端
---

# 多终端桌面实现

## 概览

本篇讲解 tag 035 中最上层的改动：多终端桌面的完整实现。涉及 gui_init.cpp 的 create_shell_terminal 重构（每次点击 Shell 图标创建独立的终端 + pipe + shell 进程）、terminal.cpp 的析构函数（关闭 pipe + waitpid 回收 shell 子进程）、以及 init.cpp 的简化（移除全局 pipe 创建逻辑）。另外还涉及 stack guard page 机制的完善和 gui_tick_callback 的多窗口 poll 支持。

关键设计决策：每个终端独立拥有 pipe 对和 shell 进程，终端关闭时负责清理所有资源。

## 架构图

```
Shell icon click → PIT tick → gui_tick_callback
  → consume_pending_icon_action → g_pending_action.store(OpenShell)
  → gui_worker thread → gui_process_pending()
    → g_pending_action.exchange(None) → create_shell_terminal()

create_shell_terminal():
  new Terminal → new Pipe(stdin) + new Pipe(stdout)
  → fork()
    → parent: set_shell_pid, add_window
    → child: new AddressSpace, new FDTable(fd0=stdin, fd1=stdout)
             execve("/bin/sh"), user stack, activate, jump_to_usermode

gui_tick_callback() [every PIT tick]:
  drain mouse events → consume icon action
  for each terminal window: poll_output() + render_to_canvas()
  composite all windows

~Terminal():
  close pipe endpoints → waitpid(shell_pid) loop → reap zombie
```

## 代码精讲

### create_shell_terminal() — pipe 创建与 fork

```cpp
void create_shell_terminal() {
    g_terminal_counter++;
    char title_buf[64];
    strcpy(title_buf, "Shell #");
    utoa(title_buf + strlen(title_buf), g_terminal_counter);

    auto* term = new Terminal(term_x, term_y, title_buf);
    term->set_font(g_font);

    auto* stdin_pipe       = new cinux::ipc::Pipe();
    auto* stdin_read_ops   = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops  = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    auto* stdout_pipe        = new cinux::ipc::Pipe();
    auto* stdout_write_ops   = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops  = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);
```

每次调用都创建全新的 pipe 对。stdin pipe 用于 Terminal -> Shell 方向（on_key 把字符写入 stdin_pipe，shell 从 fd 0 读取），stdout pipe 用于 Shell -> Terminal 方向（shell 向 fd 1 写入，Terminal 的 poll_output 从 stdout_pipe 读取）。

pipe 需要包装成 Inode 才能放进 FDTable——因为 FDTable 存的是 File 对象，File 引用的是 Inode。PipeReadOps 和 PipeWriteOps 分别封装了 pipe 的读端和写端操作。

### create_shell_terminal() — fork 与子进程 execve

```cpp
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid > 0) {
        term->set_shell_pid(child_pid);
        // ... add_window below ...
    } else if (child_pid == 0) {
        __asm__ volatile("cli");
        auto* task = cinux::proc::Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();

        task->fd_table = new cinux::fs::FDTable();
        task->fd_table->set(0, new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY));
        task->fd_table->set(1, new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY));

        auto result = cinux::proc::execve("/bin/sh", argv, envp);
        if (result != cinux::proc::ExecveResult::Ok) Scheduler::exit_current();

        uint64_t entry = task->ctx.rip;
        // ... alloc user stack pages, map ...
        task->addr_space->activate();
        cinux::proc::g_per_cpu.update_syscall_stack(task->kernel_stack_top);
        jump_to_usermode(entry, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
        Scheduler::exit_current();
    }
    wm.add_window(term);
```

子进程的关键操作：创建新 FDTable 并绑定 stdin read（fd 0）和 stdout write（fd 1）。这样每个 shell 有自己独立的 I/O 通道——输入来自自己终端的键盘，输出到自己的终端窗口，不会和其他 shell 串数据。

### gui_tick_callback — 多窗口 poll

```cpp
void gui_tick_callback(void* /*ctx*/) {
    // ... drain mouse events ...
    IconAction action = wm.consume_pending_icon_action();
    if (action != IconAction::None) {
        g_pending_action.store(action, std::memory_order_release);
    }

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

遍历所有窗口，对每个 terminal 窗口执行 poll_output（从 stdout pipe 读取 shell 输出）和 render_to_canvas（渲染到后备缓冲区）。最后 composite 把所有窗口合成到屏幕上。之前只 poll focused 窗口——多终端场景下后台 shell 的输出会被遗漏。

### Terminal 析构函数 — pipe 关闭与 zombie 回收

```cpp
Terminal::~Terminal() {
    if (stdin_pipe_ != nullptr) stdin_pipe_->close_writer();
    if (stdout_pipe_ != nullptr) stdout_pipe_->close_reader();
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;

    if (shell_pid_ > 0) {
        for (uint32_t attempt = 0; attempt < 1000; attempt++) {
            int status = 0;
            auto result = cinux::proc::waitpid(shell_pid_, &status,
                                                cinux::proc::g_pid_alloc);
            if (result == cinux::proc::WaitpidResult::Ok) {
                cinux::lib::kprintf("[TERM] Reaped shell pid=%d status=%d\n",
                        shell_pid_, status);
                break;
            }
            if (result == cinux::proc::WaitpidResult::NoChildren ||
                result == cinux::proc::WaitpidResult::NotFound) break;
        }
        shell_pid_ = 0;
    }
}
```

关闭 pipe 的方向要正确：Terminal 是 stdin pipe 的 writer（键盘 -> pipe -> shell），所以关闭 writer 端让 shell 的 sys_read 返回 EOF。Terminal 是 stdout pipe 的 reader（shell -> pipe -> Terminal），所以关闭 reader 端让 shell 的 sys_write 失败。

waitpid 用循环重试——因为 waitpid 是非阻塞的（子进程还没退出就返回 NotExited）。pipe 关闭后 shell 通常很快退出变成 Zombie，循环很快就能回收。1000 次上限防止 shell 卡住时无限循环。

### kernel_init_thread 简化

```cpp
void kernel_init_thread() {
    // ... ext2 mount, VFS init ...

#ifdef CINUX_GUI
    cinux::lib::kprintf("[INIT] ===== Milestone 035: Multi-Terminal =====\n");
    cinux::gui::gui_start();  // 注册图标 + tick callback
    auto* gui_task = TaskBuilder().set_entry(gui_worker_thread)
                                  .set_name("gui_worker").build();
    if (gui_task != nullptr) {
        Scheduler::add_task(gui_task);
        cinux::lib::kprintf("[INIT] GUI worker thread launched\n");
    }
#else
    // Non-GUI: fork shell directly
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid == 0) {
        // ... execve, user stack, jump_to_usermode ...
    }
#endif

    Scheduler::exit_current();
}
```

GUI 模式下 init 线程只做三件事：挂载 ext2、启动 GUI（注册图标和 tick callback）、启动 gui_worker 线程。pipe 创建和 fork+execve 的职责完全下沉到 create_shell_terminal()，init 不再关心。非 GUI 模式保留了直接 fork+execve 的路径，用 legacy sys_read/sys_write 代替 pipe。

### exception_handlers.cpp — Stack Guard Page 检测

```cpp
void handle_pf(InterruptFrame* frame) {
    // ... read CR2 ...

    // Scheduler task guard page
    auto* cur = Scheduler::current();
    if (cur != nullptr && cur->kernel_stack_guard_page != 0) {
        uint64_t guard_base = cur->kernel_stack_guard_page;
        uint64_t guard_end  = guard_base + PAGE_SIZE;
        if (fault_addr >= guard_base && fault_addr < guard_end) {
            kprintf("  KERNEL STACK OVERFLOW DETECTED\n");
            kprintf("  Task: tid=%u pid=%d name='%s'\n", ...);
            kpanic("kernel stack overflow: ...");
        }
    }

    // Boot stack guard
    if (cur == nullptr) {
        uint64_t guard_start = reinterpret_cast<uint64_t>(__boot_guard_start);
        uint64_t guard_end   = reinterpret_cast<uint64_t>(__boot_guard_end);
        if (fault_addr >= guard_start && fault_addr < guard_end) {
            kprintf("  BOOT STACK OVERFLOW DETECTED\n");
            kpanic("boot stack overflow: ...");
        }
    }
    // ... demand-paging, CoW, fatal ...
}
```

两层检测：scheduler task 的 guard page（通过 TCB 的 kernel_stack_guard_page 字段）和 boot stack 的 guard 区域（通过 linker symbol __boot_guard_start/end）。检测到 overflow 时打印详细诊断信息（fault 地址、guard 范围、栈范围、当前 RSP/RIP），然后 kpanic 终止系统。

## 设计决策

### 决策：延迟工作队列 vs ISR 直接 fork

**问题**: PIT tick callback 中可以直接 fork 吗？
**做法**: 不行。用 Atomic deferred action queue：ISR 设置标志，gui_worker 线程执行实际 fork。
**原因**: PIT tick 在 Interrupt gate 下运行（IF=0），此时拿 VMM/PMM 的锁会导致死锁（fork 需要分配页表页）。gui_worker 是普通内核线程，在中断开启状态下运行，可以安全地执行 fork+execve。

## 扩展方向

- 终端拖拽和调整大小
- 多终端间的复制粘贴（通过 clipboard buffer）
- 终端关闭确认（如果 shell 还在运行则弹确认框）
- 终端 tab 支持（一个窗口内多个 shell session）

## 参考资料
- Linux PTY (pseudo-terminal): https://man7.org/linux/man-pages/man7/pty.7.html
- SerenityOS Terminal: https://github.com/SerenityOS/serenity/tree/master/Userland/Applications/Terminal
- OSDev Wiki IPC: https://wiki.osdev.org/Message_Passing
