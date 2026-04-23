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