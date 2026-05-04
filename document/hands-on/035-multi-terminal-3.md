# 多终端桌面与 init 重构

## 导语

前面两篇我们打通了 fork + execve + COW 的完整链路，一个 Shell 进程能从 ext2 加载并在用户态运行了。但一个操作系统只有一个终端也太寒酸了——这一篇要把桌面升级为多终端：每次点击 Shell 图标就创建一个独立的终端窗口，每个终端绑定自己的 shell 进程和 pipe 对，互不干扰。

在这个过程中，我们还要重构 init 线程和 Terminal 类的生命周期管理——终端关闭时要回收 shell 子进程（防止 zombie），init 线程不再全局创建 pipe 而是把责任下沉到 create_shell_terminal()。另外我们还要完善 stack guard page 机制，防止内核栈溢出时静默踩坏内存。

完成本篇后，Cinux 桌面将真正拥有多任务能力——多个 shell 终端并发运行，各自独立。

## 概念精讲

### 多终端架构

多终端的关键设计是"每个终端独立拥有自己的 pipe 对和 shell 进程"。之前的架构是全局共享一对 pipe（所有终端的输入输出都经过同一个 pipe），这在多 shell 场景下会互相串数据。新架构中 create_shell_terminal() 每次调用都创建新的 stdin pipe 和 stdout pipe，fork 子进程后把 pipe 的读端绑到 fd 0（shell 读 stdin）、写端绑到 fd 1（shell 写 stdout），父进程把 pipe 对象绑到 Terminal 对象。

### 终端生命周期

终端从创建到销毁经历：new Terminal() -> set_stdin_pipe/set_stdout_pipe -> fork shell -> add_window -> 正常运行（poll_output + render_to_canvas）-> 用户关闭 -> ~Terminal()。析构函数负责关闭 pipe 端点（stdin_pipe_->close_writer() 让 shell 读到 EOF，stdout_pipe_->close_reader() 让 shell 写入失败）和回收 shell 子进程（循环调用 waitpid 直到成功或超时）。

### Stack Guard Page

每个内核栈下面有一个 guard page（4KB，故意不映射）。栈溢出时写入 guard page 触发 #PF，handler 检查 fault 地址是否在 guard 范围内，如果是就报告 "STACK OVERFLOW" 然后 panic。这比静默踩坏内存好得多——至少你能知道出了什么事。对于 boot stack（测试用的栈），guard 区域在 linker script 中定义，运行时需要显式 unmap。

## 动手实现

### Step 1: create_shell_terminal() 重构

**目标**: 实现完整的 per-terminal pipe 创建和 fork+execve 流程。

**设计思路**: 每次调用 create_shell_terminal 都创建独立的资源。流程：生成唯一标题（Shell #1, #2, ...） -> new Terminal() -> 创建 stdin pipe + stdout pipe + 对应的 inode 和 ops -> 绑定 pipe 到 terminal -> fork()。父进程：记录 shell PID，add_window。子进程：cli -> 创建 AddressSpace -> 创建私有 FDTable（fd 0 = stdin read, fd 1 = stdout write）-> execve("/bin/sh") -> 设置用户栈 -> activate -> update_syscall_stack -> jump_to_usermode。

**实现约束**: stdin pipe 的 read 端用 PipeReadOps 包装成 inode，绑定到子进程 fd 0。stdout pipe 的 write 端用 PipeWriteOps 包装成 inode，绑定到子进程 fd 1。Terminal 对象通过 set_stdin_pipe/set_stdout_pipe 获得直接的 pipe 指针，用于 on_key() 写入和 poll_output() 读取。Terminal 还通过 set_shell_pid 记录子进程 PID。

**踩坑预警**: fork 失败时要清理所有已分配的资源——delete pipe ops、inode、pipe 本身、Terminal 对象，然后 return。

### Step 2: gui_tick_callback 多窗口 poll

**目标**: 修改 tick callback 遍历所有终端窗口执行 poll_output。

**设计思路**: 之前的 tick callback 只 poll focused 窗口的输出。多终端场景下每个 shell 都在后台往自己的 stdout pipe 写数据，如果只 poll 一个窗口，其他 shell 的输出就会堆积在 pipe buffer 里不显示。新逻辑：遍历 wm 的所有窗口，对每个 is_terminal() 的窗口调用 poll_output() 和 render_to_canvas()。

**实现约束**: 用 for 循环遍历 wm.window_count()，对每个 window_at(i) 检查 is_terminal()，如果是就 static_cast 为 Terminal* 并调用 poll_output() + render_to_canvas()。这放在 composite() 之前，确保 compositing 时数据是最新的。

### Step 3: Terminal 析构与 zombie 回收

**目标**: 实现 Terminal 的析构函数，关闭 pipe 并回收 shell 子进程。

**设计思路**: Terminal 被销毁时（通常是窗口管理器 remove_window 后 delete），需要做两件事：关闭 pipe 端点让 shell 知道终端没了，以及 waitpid 回收 shell 子进程防止 zombie。关闭 stdin 的 writer 端让 shell 的 sys_read 返回 0（EOF），shell 会因此退出。关闭 stdout 的 reader 端让 shell 的 sys_write 失败，shell 也会因此退出。然后循环调用 waitpid 尝试回收——shell 通常很快退出变成 Zombie，waitpid 就能成功回收。

**实现约束**: 析构函数中：if stdin_pipe_ != nullptr then stdin_pipe_->close_writer()，if stdout_pipe_ != nullptr then stdout_pipe_->close_reader()，两个指针置 nullptr。然后 if shell_pid_ > 0，循环最多 1000 次 waitpid(shell_pid_, &status, g_pid_alloc)，成功就 break。

**踩坑预警**: waitpid 是非阻塞的——如果子进程还没退出就返回 NotExited。所以需要循环重试。但也不能无限循环（万一 shell 卡住了），所以加一个上限。1000 次足够了——通常 shell 在 pipe 关闭后几乎立即退出。

### Step 4: kernel_init_thread 重构

**目标**: 移除全局 pipe 创建逻辑，pipe 创建责任下沉到 create_shell_terminal()。

**设计思路**: 之前的 init 线程在 gui_start() 之前创建全局 pipe 对，然后通过 set_shell_pipes() 接口传给 Terminal。现在 init 线程只需调用 gui_start()（注册桌面图标和 tick callback），启动 gui_worker 线程，就完事了。pipe 创建完全由 create_shell_terminal() 负责。非 GUI 模式的 fork+execve 路径保留在 init 线程中。

**实现约束**: GUI 模式下：gui_start() 注册图标和 callback -> 创建 gui_worker 线程 -> Scheduler::add_task(gui_task)。移除所有全局 pipe 创建代码和 set_shell_pipes() 调用。非 GUI 模式下：直接 fork -> 子进程创建 AddressSpace -> execve -> 设置用户栈 -> jump_to_usermode（完整内联版本）。

### Step 5: Stack Guard Page 完善

**目标**: 在 #PF handler 中添加 boot stack guard page 检测。

**设计思路**: 之前 #PF handler 有 scheduler task 的 guard page 检测（检查 fault 地址是否在 task->kernel_stack_guard_page 范围内）。现在增加 boot stack 的 guard page 检测——当没有 scheduler task（cur == nullptr）时，检查 fault 地址是否落在 __boot_guard_start 和 __boot_guard_end 之间（由 linker script 定义）。

**实现约束**: 在 handle_pf 中，如果 cur != nullptr 且 cur->kernel_stack_guard_page != 0，检查 scheduler task 的 guard page。如果 cur == nullptr，检查 boot stack guard（使用 extern "C" 声明的 linker symbol __boot_guard_start、__boot_guard_end、__kernel_stack_top）。检测到 overflow 时打印详细的诊断信息（fault 地址、guard 范围、栈范围、当前 RSP、RIP）然后 kpanic。

**踩坑预警**: Guard page 要在运行时显式 unmap 才生效——光在 linker script 里留空间不够，因为 boot stack 用的是 2MB huge page 映射，整个区域都可访问。需要在测试初始化代码中把 guard 区域 split_2mb_page + unmap。不过这是测试框架的职责，handler 只负责检测。

## 构建与运行

```bash
cmake --build build
make run
```

在 QEMU 中点击 Shell 图标应该弹出一个终端窗口，再点一次弹出第二个，两个终端各自独立运行 shell。

## 调试技巧

1. **多终端输出互相串**: 检查每个终端是否绑定了独立的 pipe 对。在 create_shell_terminal 中打印 pipe 指针地址，确认每次都不同。

2. **关闭一个终端后另一个也死了**: Terminal 析构函数可能关闭了不该关闭的 pipe 端点。确保析构函数只关闭自己绑定的 pipe（stdin_pipe_ 和 stdout_pipe_），不要关 fd_table 里的 file。

3. **测试卡死无输出**: 可能是栈溢出。检查 Terminal 对象大小——screen_[80][25] 的 TerminalCell 数组约 24KB，加上 Pipe buffer 约 32KB，远超 16KB 内核栈。改用堆分配或增大栈。

## 本章小结

| 组件 | 关键设计 | 要点 |
|------|----------|------|
| create_shell_terminal | per-terminal pipe + fork + execve | 每次调用独立资源 |
| gui_tick_callback | 遍历所有 terminal 窗口 poll | 不只 poll focused |
| Terminal 析构 | close pipe + waitpid 循环回收 | 防止 zombie |
| init 重构 | 移除全局 pipe，pipe 下沉 | GUI 模式和非 GUI 模式分叉 |
| Stack guard | #PF 检测 guard page + panic | boot stack 和 scheduler task 双重检测 |
