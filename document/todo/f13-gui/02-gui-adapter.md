# M2: 内核侧 Thin Adapter 实现

> 实现 gui_abi.hpp 定义的所有接口，转发到内核内部服务。
> 这是内核侧的"胶水代码"。

## 任务清单

### T1: 渲染 Adapter

**文件**: `kernel/gui/gui_adapter.cpp`

```cpp
// Framebuffer adapter
GuiFramebuffer* gui_get_framebuffer() {
    return (GuiFramebuffer*)&g_framebuffer;
}

// Canvas adapter
GuiCanvas* gui_canvas_create(uint32_t w, uint32_t h) {
    auto* canvas = new Canvas();
    canvas->init(w, h);
    return (GuiCanvas*)canvas;
}
```

- [ ] Framebuffer 转发到 `cinux::drivers::Framebuffer`
- [ ] Canvas 转发到 `cinux::drivers::Canvas`
- [ ] Font 转发到 `cinux::drivers::PSFFont`

### T2: 事件 Adapter

```cpp
bool gui_event_dequeue(GuiEvent* out) {
    auto& eq = cinux::gui::EventQueue::instance();
    return eq.dequeue(*(cinux::gui::Event*)out);
}
```

- [ ] Event 类型映射（内核 GuiEvent → ABI GuiEvent，结构相同）
- [ ] 键盘 IRQ → event queue enqueue
- [ ] 鼠标 IRQ → event queue enqueue

### T3: Shell Adapter

```cpp
struct GuiShellInternal {
    cinux::ipc::Pipe* stdin_pipe;
    cinux::ipc::Pipe* stdout_pipe;
    int child_pid;
    bool alive;
};

GuiShell* gui_shell_spawn(const char* path) {
    auto* s = new GuiShellInternal();
    s->stdin_pipe = new cinux::ipc::Pipe();
    s->stdout_pipe = new cinux::ipc::Pipe();

    // 创建 pipe inode + fd
    // fork + execve
    // 设置 stdin/stdout pipe
    // 返回句柄
}
```

- [ ] gui_shell_spawn — 封装 fork + execve + pipe 创建
- [ ] gui_shell_write_stdin — 转发到 stdin_pipe
- [ ] gui_shell_read_stdout — 转发到 stdout_pipe
- [ ] gui_shell_wait — 封装 waitpid
- [ ] gui_shell_destroy — 关闭 pipe + 回收

### T4: 定时器 / 内存 / 日志 Adapter

- [ ] gui_register_tick → `PIT::set_tick_callback()`
- [ ] gui_alloc → `kmalloc`
- [ ] gui_free → `kfree`
- [ ] gui_log → `kprintf`

## 产出物

- [ ] `kernel/gui/gui_adapter.cpp` — 全部 ABI 实现
- [ ] 链接到 big_kernel_common
