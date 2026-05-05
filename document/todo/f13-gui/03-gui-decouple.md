# M3: 解耦现有 GUI 代码

> 将现有 `kernel/gui/` 代码从直接内核调用改为通过 ABI 层。
> 完成后 GUI 代码只依赖 gui_abi.hpp，可提取为独立仓库。

## 任务清单

### T1: WindowManager 解耦

**文件**: `kernel/gui/window_manager.hpp` / `.cpp`

当前直接依赖：Canvas, Mouse, Event
改为：通过 gui_abi 渲染和输入接口

- [ ] Canvas 引用 → `GuiCanvas*`
- [ ] Mouse::x()/y() → `gui_mouse_x()/gui_mouse_y()`
- [ ] EventQueue → `gui_event_dequeue()`
- [ ] 移除 `#include "kernel/drivers/canvas.hpp"` 等内核头文件

### T2: Terminal 解耦

**文件**: `kernel/gui/terminal.hpp` / `.cpp`

当前直接依赖：Pipe, Process, PID, Font
改为：通过 `GuiShell*` 接口

- [ ] Pipe/Process 管理 → `gui_shell_spawn/read/write/wait/destroy`
- [ ] PSFFont → `GuiFont*`
- [ ] 移除 `#include "kernel/ipc/pipe.hpp"` 等内核头文件

### T3: Window 解耦

**文件**: `kernel/gui/window.hpp` / `.cpp`

当前直接依赖：Canvas, Event
改为：通过 gui_abi 渲染接口

- [ ] Canvas → `GuiCanvas*`
- [ ] Event → `GuiEvent`

### T4: GUI Init 解耦

**文件**: `kernel/gui/gui_init.hpp` / `.cpp`

当前直接调用 fork/execve/PMM/VMM 等。
改为：
- 初始化 GUI 系统时调用 adapter 注册
- Shell 启动通过 `gui_shell_spawn()`

- [ ] gui_init 简化为：获取 framebuffer + 注册 tick + 启动 shell
- [ ] 移除直接 fork/execve/PMM 调用

### T5: GUI 仓库提取准备

- [ ] 确认 GUI 代码只 `#include "gui_abi.hpp"`
- [ ] 创建独立 CMakeLists.txt（可在 host Linux 上 mock 测试）
- [ ] 平台抽象层：
  ```
  libs/gui/
  ├── include/gui_abi.hpp     # ABI 定义
  ├── src/                     # GUI 核心代码
  │   ├── window_manager.cpp
  │   ├── terminal.cpp
  │   ├── window.cpp
  │   └── event.cpp
  ├── platform/
  │   ├── kernel/adapter.cpp   # 内核 adapter（留在内核树）
  │   └── host/mock.cpp        # Linux mock（独立测试）
  └── test/
  ```

### T6: 验证

- [ ] GUI 功能不回归（窗口渲染、鼠标移动、Shell 终端）
- [ ] GUI 代码编译不依赖任何内核内部头文件
- [ ] host mock 层可以独立编译测试

## 产出物

- [ ] 解耦后的 GUI 代码（仅依赖 gui_abi.hpp）
- [ ] 独立仓库目录结构准备
- [ ] host mock 测试框架
- [ ] 内核侧 adapter 留在内核树
