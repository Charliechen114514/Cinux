---
title: 031-gui-native-app-3 · 原生应用
---

# 031-3: 系统集成 — sys_read/write 重构与 GUI Shell 启动

## 概览

本文是 tag 031 通读系列的第三篇，聚焦于将 pipe 和 terminal 两个子系统集成为 GUI shell 终端的系统调用层支持。涉及的改动分布在多个文件中：sys_read/sys_write 的路由重构、FDTable 的 set 扩展、gui_init.cpp 中的 gui_start 终端创建流程、以及 PIT tick 回调的终端轮询支持。

这些改动虽然分散，但它们共同解决了一个问题：让 pipe fd 能通过标准系统调用路径读写，并在 GUI 模式下展示终端窗口。

## 架构图

```
gui_init.cpp: gui_start()
         |
         v
    Terminal window
    stdin_pipe (try_write)  <-- on_key()
    stdout_pipe (try_read)  --> poll_output()
         |
    render_to_canvas -> Canvas
         |
    WindowManager::composite -> Screen

sys_read(0)  --> g_global_fd_table() --> InodeOps::read --> Pipe::read
sys_write(1) --> g_global_fd_table() --> InodeOps::write --> Pipe::write
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
    cinux::fs::FDTable& tbl  = cinux::fs::g_global_fd_table();
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
    cinux::fs::FDTable& tbl  = cinux::fs::g_global_fd_table();
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

### gui_start 终端创建 (gui/gui_init.cpp)

gui_start 函数负责创建终端窗口并注册 PIT tick 回调。在 tag 031 中，终端创建是简洁的——只创建一个 Terminal 对象并加入窗口管理器，不涉及 fork/execve/管道（这些将在后续 tag 中添加）。

```cpp
Terminal* gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 030: GUI Window Manager =====\n");

    cinux::drivers::Mouse::init();

    if (g_screen != nullptr) {
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(),
                                                  g_screen->height());
    }

    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400
    uint32_t term_x = 80;
    uint32_t term_y = 60;

    if (g_screen != nullptr) {
        uint32_t sw = g_screen->width();
        uint32_t sh = g_screen->height();
        if (term_w + 80 < sw) term_x = (sw - term_w) / 2;
        if (term_h + 60 < sh) term_y = (sh - term_h) / 2;
    }

    auto* term = new Terminal(term_x, term_y, "Cinux Terminal");
    term->set_font(g_font);
    WindowManager::instance().add_window(term);

    cinux::lib::kprintf("[GUI] WindowManager initialised with Terminal window.\n");

    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
    cinux::lib::kprintf("[GUI] GUI tick callback registered on PIT.\n");

    return term;
}
```

gui_start 首先初始化 PS/2 鼠标驱动并设置屏幕边界。然后计算终端窗口的位置——默认偏移 (80, 60)，如果屏幕空间足够则居中。创建 Terminal 对象后设置字体并加入窗口管理器。最后注册 PIT tick 回调，返回终端指针供调用方（如 kernel_init_thread）使用。

### PIT tick 回调中的终端轮询 (gui/gui_init.cpp)

```cpp
void gui_tick_callback(void* /*ctx*/) {
    // ... event processing ...

    // Poll the focused terminal for shell output (if it has a stdout pipe)
    auto* focused = wm.focused();
    if (focused != nullptr) {
        auto* term = static_cast<Terminal*>(focused);
        term->poll_output();
        term->render_to_canvas();
    }

    // Composite all windows onto the screen
    wm.composite();
}
```

tick 回调在每次 PIT 中断时执行（约 100Hz）。终端轮询逻辑只处理当前焦点窗口：从 stdout 管道读取 shell 输出（poll_output）并渲染到 Canvas（render_to_canvas），最后 composite 把所有窗口合成到屏幕。static_cast 是安全的，因为在 tag 031 中所有窗口都是 Terminal 实例。

### 窗口管理器的 Terminal 支持 (gui/window_manager.cpp)

WindowManager 的改动包括焦点管理（focused() 方法）和键盘事件路由到焦点窗口。handle_key 将 KeyEvent 转发到当前焦点窗口的 on_key 方法，让 Terminal 直接接收键盘输入。composite() 方法负责将所有窗口的 Canvas 合成到屏幕上。

## 设计决策

### 决策：VFS 优先的系统调用路由
**问题**: sys_read/sys_write 怎么同时支持管道和传统设备？
**本项目的做法**: 先查 FDTable 的 VFS 条目，有则走 InodeOps，否则回退硬编码路径。
**备选方案**: 为管道 fd 增加专门的分支（如检查 fd 编号或文件类型）。
**为什么不选备选方案**: VFS 优先路由是更通用的设计——未来加入 socket、设备文件等也不需要改系统调用代码。InodeOps 多态已经提供了正确的分发机制。
**如果要扩展/改进**: 增加更多文件类型（socket、block device）时不需要修改 sys_read/sys_write，只需在 FDTable 中安装对应的 InodeOps。

### 决策：焦点窗口轮询而非遍历所有窗口
**问题**: tick 回调中应该轮询所有终端窗口还是只轮询焦点窗口？
**本项目的做法**: 只轮询当前焦点窗口的 poll_output 和 render_to_canvas。
**备选方案**: 遍历所有窗口，对 Terminal 类型的窗口都执行轮询。
**为什么不选备选方案**: tag 031 只有一个终端窗口，遍历开销虽然不大但没必要。焦点窗口轮询更简单直接。
**如果要扩展/改进**: 支持多终端窗口时改为遍历所有窗口轮询，确保每个终端都能及时刷新输出。

## 扩展方向

- **Shell 进程 fork/execve 集成**: 在 gui_start 中 fork 子进程并 execve /bin/sh，通过管道连接终端 (难度: 较高)
- **多窗口 shell**: 支持同时运行多个终端窗口，每个独立 shell 会话 (难度: 中等)
- **信号传递**: 支持 SIGPIPE（写端已关闭时通知 shell）和 SIGHUP（终端关闭时通知 shell） (难度: 中等)
- **伪终端 (pty)**: 实现 ptmx/pts 设备，支持 SSH 等远程终端 (难度: 较高)
- **dup2 系统调用**: 支持文件描述符重定向，让 shell 能实现管道操作符 (难度: 中等)

## 参考资料

- OSDev Wiki: [Unix Pipes](https://wiki.osdev.org/Unix_Pipes) — 管道的基本概念和 fork/execve/pipe 三件套
- xv6 RISC-V: [pipe.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c) — 经典教学 OS 的管道和 fork/execve 集成
