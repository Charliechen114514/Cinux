# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☐` 打完后打 tag，触发 prompts/ 工作流

---

## Phase 8 · 存储与文件系统

---

## Phase 9 · GUI

> **CLI/GUI 双模式架构**：通过 CMake 编译选项 `CINUX_GUI`（默认 `ON`）控制。
> - **GUI 模式**（`ON`）：编译 Canvas、WindowManager、鼠标驱动等 GUI 组件，`kernel_main` 启动图形栈。
> - **CLI 模式**（`OFF`）：仅保留 Framebuffer + Console（纯文字输出），不编译鼠标/WM/双缓冲代码，镜像更精简。
> - 两种模式共享 Framebuffer 驱动和 PSF2 字体渲染；CLI 模式下 Console 直接写 front buffer（无双缓冲开销）。

### `030_gui_wm_basic`
**效果**：GUI 模式下可拖动窗口，Z-order 正确；CLI 模式下行为不变

**PS/2 鼠标驱动**
- ☐ `kernel/drivers/mouse.hpp`：`class Mouse { x_, y_, buttons_; }`；`init()`（8042 CMD `0xA8` 启用鼠标、`0x20` 读配置→bit1 置1→`0x60` 写回、CMD `0xD4` 发 `0xF4` 给鼠标激活）、IRQ12 handler 解析 3 字节包（byte0=buttons, byte1=dx, byte2=dy），`poll(MouseEvent& out)` 获取事件
- ☐ `MouseEvent {dx, dy, buttons, left, right, middle}`；驱动内部维护 `mouse_x_/y_`，clamp 到屏幕范围
- ☐ CLI 模式兼容：鼠标驱动不参与编译，IRQ12 不注册

**Window 与事件体系**
- ☐ `kernel/gui/window.hpp`：`class Window {x_,y_,w_,h_,title_[64],*canvas_,visible_,focused_,id_}`；`draw_title_bar()`、`draw_content()`
- ☐ `kernel/gui/event.hpp`：`enum class EventType {MouseMove, MouseDown, MouseUp, KeyDown, KeyUp}`；`struct Event { type_, union{MouseEvent,KeyEvent} }`；环形事件队列 `EventQueue {buf_[128], head_, tail_}`
- ☐ `kernel/gui/window_manager.hpp`：`class WindowManager { windows_[64], count_, *focused_, mouse_x_, mouse_y_ }`；`create(title,w,h)`、`destroy(id)`、`raise(id)`、`composite()`、`handle_mouse(Event&)`、`handle_key(Event&)`

**合成与交互**
- ☐ `composite()`：从低 Z-order 到高，依次 blit 各 window canvas 到屏幕 Canvas back_buf，最后 `flip()`
- ☐ 拖动：`handle_mouse` 检测 left button 按下+移动，更新 focused window 的 x/y，每帧 `composite()` 刷新
- ☐ 标题栏：蓝色背景 + 白色标题文字 + 关闭按钮（红色小方块），点击关闭按钮调用 `destroy()`

**kernel_main 集成**
- ☐ `#ifdef CINUX_GUI`：GUI 模式下 `kernel_init_thread` 启动 WM 初始化（`WindowManager::init(canvas)`）；PIT tick 中调 `composite()`
- ☐ 键盘事件双路分发：GUI 模式下键盘 IRQ 同时写入 EventQueue（供 WM）和 key_queue（供 CLI fallback）

**测试**
- ☐ Host 单元测试 `test_mouse.cpp`：mock 8042 I/O，验证 3 字节包解析（dx/dy/buttons 正确）
- ☐ Host 单元测试 `test_event_queue.cpp`：验证 enqueue/dequeue 环形缓冲、满/空边界
- ☐ Host 单元测试 `test_window_manager.cpp`：mock Canvas，验证 create/destroy/raise Z-order、composite blit 区域
- ☐ QEMU 内核测试：GUI 模式下创建 3 个窗口，鼠标拖动验证位置更新，点击关闭按钮验证 destroy；CLI 模式下测试行为不变

---

### `031_gui_native_app`
**效果**：GUI 模式下屏幕出现可交互的终端模拟器窗口，shell 在其中运行；CLI 模式下行为不变

**Terminal 窗口**
- ☐ `user/programs/terminal/main.cpp`：`Terminal` 继承 `Window`；`screen_[80][25]` 字符缓冲，`cursor_x_/y_`，`fg_/bg_` 颜色状态
- ☐ `on_key(KeyEvent&)`：转发按键给 shell 进程（通过 pipe 或共享内存 IPC）
- ☐ `on_paint()`：遍历 `screen_` 调 `canvas_->draw_text()`，光标用反色块渲染
- ☐ `write(const char* str, size_t len)`：shell 输出回调，更新 `screen_` 字符缓冲并触发 `on_paint()`
- ☐ 标题栏 `Cinux Terminal`，最小化/关闭按钮占位（关闭按钮调用 WM `destroy()`）

**Shell 集成**
- ☐ 复用现有 `user/programs/shell/` 代码，shell stdin/stdout 重定向到 Terminal 的 pipe 端点
- ☐ `kernel_init_thread`：GUI 模式下创建 Terminal 窗口 + 启动 shell 进程；CLI 模式下直接启动 shell（现有路径不变）
- ☐ WM 注册 terminal 窗口，键盘事件由 WM 路由到 focused window（Terminal）

**kernel_main 集成**
- ☐ `#ifdef CINUX_GUI`：GUI 模式下 `kernel_init_thread` 走 Terminal 窗口路径；CLI 模式下走原有裸 shell 路径

**测试**
- ☐ Host 单元测试 `test_terminal.cpp`：mock Canvas + pipe，验证 on_key 输入→screen 缓冲更新→on_paint 输出正确
- ☐ Host 单元测试 `test_shell_redirect.cpp`：验证 shell stdout 写入 pipe→Terminal::write 回调→screen 更新
- ☐ QEMU 内核测试：GUI 模式下 Terminal 窗口可输入命令（`echo`/`help`/`clear`），输出正确渲染，关闭按钮退出 shell；CLI 模式下测试行为不变

---

## 附录 · AI Checkpoint 工作流

```
☑ 所有 checkbox 完成
  → git tag 编号_大主题_小阶段
  → 复制本 milestone 块 → prompts/03_code_review.md {{code_snippet}}
  → 复制本 milestone 块 → prompts/04_test_generation.md {{interface_snippet}}
  → 测试全绿
  → 复制本 milestone 块 → prompts/01_tutorial_hands_on.md
  → 复制本 milestone 块 → prompts/02_tutorial_readthrough.md（附完整代码）
  → 更新本文件 ☐→☑，更新 README.md 进度表
```

### 占位符速填

| 占位符 | 来源 |
|--------|------|
| `{{current_tag}}` | 刚打的 git tag |
| `{{prev_tag}}` | `git tag` 列表倒数第二 |
| `{{phase_title}}` | milestone `###` 标题 |
| `{{milestone_goal}}` | 本节「效果」一行 |
| `{{key_files}}` | 本节所有「涉及文件」 |
| `{{checklist_items}}` | 本节所有 `☐` 条目 |
| `{{code_snippet}}` | 你写完的实际代码 |