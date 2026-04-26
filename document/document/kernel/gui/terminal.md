# Terminal 终端

> 里程碑: `031_gui_native_app` `035_multi_terminal`

## 功能概述

GUI 模式下的终端模拟器窗口，通过 pipe 连接独立 shell 进程。支持多终端并发，每个终端绑定独立的 fork+execve shell 子进程。仅在 `CINUX_GUI` 模式下编译。

## Terminal 窗口 (`kernel/gui/terminal.hpp/cpp`)

### 内部结构
- 继承 `Window`，80x25 `screen_` 字符缓冲
- 光标管理 + fg/bg 颜色
- pipe 指针: stdin_pipe / stdout_pipe

### 核心方法
- `on_key(KeyEvent&)` — 按键写入 pipe (shell stdin)
- `on_paint(Canvas&)` — 遍历 screen_ 调 canvas_->draw_text()，光标反色块
- `write(data)` — 处理控制字符 (换行、退格、滚动)
- `is_terminal() const override { return true; }` — 类型识别

### 关闭流程
- 关闭按钮 → close pipe reader/writer
- `waitpid` 回收 shell 子进程
- 防止 zombie

## 多终端 (035)

### 创建流程 (`kernel/gui/gui_init.cpp`)
1. `create_shell_terminal()` 被调用
2. 动态创建新 pipe 对 (`sys_pipe`)
3. `fork()` → 子进程 `execve("/bin/sh")`
4. 父进程将 pipe fd 绑定到新 Terminal
5. `add_window(terminal)` 加入 WM

### tick 回调
- `gui_tick_callback` 遍历所有窗口
- 对每个 `is_terminal()` 的窗口执行 `poll_output()` + `render_to_canvas()`
- 不只是 focused 窗口，所有终端都会更新

## Shell 启动时序
- Shell 进程在 boot 时通过 `launch_first_user` 启动
- Pipe 在 `init.cpp` 创建并绑定 fd 0/1
- Terminal 窗口延迟到用户点击 Shell 图标时才创建
- Shell 输出暂存 pipe buffer，Terminal 连接后通过 `poll_output` 取回

## 源码位置
- `kernel/gui/terminal.hpp/cpp` — Terminal 窗口
- `kernel/gui/gui_init.hpp/cpp` — GUI 初始化 & tick 回调
