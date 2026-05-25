---
title: VSCode 调试配置
---

# 005-08: VSCode 调试配置

## 概述

Cinux 项目配置了完整的 VSCode 调试环境，支持通过 GDB 远程调试 QEMU 中运行的内核。配置文件位于 [`.vscode/`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/) 目录。

## 配置文件结构

```
.vscode/
├── launch.json      # 调试配置（F5 启动）
├── tasks.json       # 构建任务（Ctrl+Shift+B）
└── gdbinit          # GDB 初始化脚本
```

---

## 1. launch.json - 调试配置

文件：[`.vscode/launch.json`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/launch.json)

### 完整配置

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "QEMU 调试 (mini kernel)",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/kernel/mini/mini_kernel",
            "cwd": "${workspaceFolder}",
            "MIMode": "gdb",
            "miDebuggerPath": "gdb",
            "miDebuggerServerAddress": "localhost:1234",
            "setupCommands": [
                {
                    "text": "-gdb-set architecture i386:x86-64"
                },
                {
                    "text": "-gdb-set disassembly-flavor intel"
                },
                {
                    "text": "-gdb-set pagination off"
                }
            ]
        },
        {
            "name": "QEMU 调试 (stage2 bootloader)",
            "type": "cppdbg",
            "request": "launch",
            "program": "/usr/bin/true",
            "cwd": "${workspaceFolder}",
            "MIMode": "gdb",
            "miDebuggerServerAddress": "localhost:1234",
            "miDebuggerPath": "gdb",
            "setupCommands": [
                {
                    "description": "加载 stage2 符号",
                    "text": "-file-exec-and-symbols ${workspaceFolder}/build/boot/stage2",
                    "ignoreFailures": true
                }
            ]
        }
    ]
}
```

### 配置项说明

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `type` | `cppdbg` | C++ 调试类型 |
| `request` | `launch` | 启动调试（非 attach） |
| `MIMode` | `gdb` | 使用 GDB 作为后端 |
| `miDebuggerPath` | `gdb` | GDB 可执行文件路径 |
| `miDebuggerServerAddress` | `localhost:1234` | QEMU GDB stub 监听地址 |
| `setupCommands` | - | GDB 启动时执行的命令 |

### setupCommands 详解

| 命令 | 说明 |
|------|------|
| `-gdb-set architecture i386:x86-64` | 设置目标架构为 x86-64 |
| `-gdb-set disassembly-flavor intel` | 使用 Intel 语法的反汇编（而非 AT&T） |
| `-gdb-set pagination off` | 禁用分页，避免输出暂停 |

---

## 2. tasks.json - 构建与运行任务

文件：[`.vscode/tasks.json`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/tasks.json)

### 关键任务

| 任务名称 | 命令 | 说明 |
|----------|------|------|
| `CMake: Configure` | `cmake -B build` | 配置 CMake 项目 |
| `CMake: Build all` | `cmake --build build` | 构建整个项目（默认） |
| `QEMU: Run debug mode` | `make -C build run-debug` | 启动 QEMU 调试模式（后台） |
| `CMake: Build & Run QEMU debug` | 顺序执行上述两个 | 先构建再启动 QEMU |
| `QEMU: Run normal` | `make -C build run` | 正常启动 QEMU（非调试） |

### QEMU 调试模式任务

```json
{
    "label": "QEMU: Run debug mode",
    "type": "shell",
    "command": "make",
    "args": ["-C", "build", "run-debug"],
    "isBackground": true,
    "group": "build",
    "problemMatcher": [],
    "detail": "启动 QEMU 调试模式（监听 :1234）"
}
```

### Make 目标定义

来源：[`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake#L71-L77)

```cmake
add_custom_target(run-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU in debug mode (GDB on :1234)"
    VERBATIM
)
```

其中 `QEMU_DEBUG_FLAGS` 定义为：

```cmake
set(QEMU_DEBUG_FLAGS
    -s      # GDB stub 监听 TCP 端口 1234
    -S      # 启动时暂停 CPU，等待 GDB 连接
)
```

---

## 3. gdbinit - GDB 初始化脚本

文件：[`.vscode/gdbinit`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/gdbinit)

```gdb
# 修复 GDB 17.x 与 QEMU 的兼容性问题
add-symbol-file build/boot/stage2
```

### 使用方式

VSCode 的 `launch.json` 会自动加载此文件（通过 GDB 的 `-x` 参数或 `.gdbinit` 默认加载）。

---

## 4. 完整调试流程

### 方法一：VSCode 图形界面调试

```
1. 打开 VSCode，进入项目目录
2. 按 F5 或选择 "Run and Debug"
3. 选择 "QEMU 调试 (mini kernel)"
4. VSCode 会：
   - 自动构建项目
   - 启动 QEMU（后台运行）
   - 连接 GDB 到 localhost:1234
   - 在入口点暂停
```

### 方法二：手动分步调试

```bash
# Terminal 1: 启动 QEMU 调试模式
cd build && make run-debug
# QEMU 会暂停，等待 GDB 连接

# Terminal 2: 启动 GDB
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
```

### 方法三：VSCode + 手动 QEMU

```bash
# Terminal: 先启动 QEMU
cd build && make run-debug

# VSCode: 按 F5 启动调试
# 会自动连接到已运行的 QEMU
```

---

## 5. 常用调试操作

| VSCode 操作 | GDB 命令 | 说明 |
|-------------|----------|------|
| `F5` | `continue` | 继续执行 |
| `F10` | `next` / `nexti` | 单步跳过 |
| `F11` | `step` / `stepi` | 单步进入 |
| `Shift+F11` | `finish` | 跳出函数 |
| `Shift+F5` | `-exec quit` | 停止调试 |

### 断点操作

| 操作 | 说明 |
|------|------|
| 点击行号左侧 | 设置/取消断点 |
| `Ctrl+Shift+F5` | 重启调试 |
| 条件断点 | 右键断点 → "Edit Breakpoint" → "Add Condition" |

---

## 6. 调试示例

### 在 mini_kernel_main 处设置断点

```cpp
// kernel/mini/test/main_test.cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    // 在这里设置断点
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    ...
}
```

VSCode 操作：
1. 打开 [`main_test.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/main_test.cpp#L20)
2. 在第 22 行点击左侧，设置断点
3. 按 F5 启动调试
4. 程序会在断点处暂停

### 查看汇编代码

1. 在 "DEBUG CONSOLE" 中输入：
```
-exec x/10i $pc
```
2. 或右键代码编辑器 → "Open Disassembly View"

---

## 7. 故障排查

### 问题：连接失败 "Connection refused"

**原因**：QEMU 未启动或未使用 `-s` 参数

**解决**：
```bash
# 确保 QEMU 正在运行并监听 1234 端口
netstat -tuln | grep 1234
# 或
lsof -i :1234
```

### 问题：符号未加载

**原因**：GDB 找不到符号文件

**解决**：
```gdb
# 手动加载符号
(gdb) file build/kernel/mini/mini_kernel
# 或
(gdb) symbol-file build/kernel/mini/mini_kernel
```

### 问题：断点不生效

**原因**：代码被优化掉或地址不匹配

**解决**：
```bash
# 确保 Debug 构建
cmake -DCMAKE_BUILD_TYPE=Debug -B build
```

---

## 8. 相关链接

- [005-09: QEMU GDB 调试](005-09-qemu-gdb-debug.md) - QEMU GDB 协议详解
- [`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake) - QEMU 配置
- [Building a VS Code Debugging Workflow for a Custom x64 OS](https://www.sqlpassion.at/archive/2025/07/22/building-a-vs-code-debugging-workflow-for-a-custom-x64-operating-system-with-qemu-and-gdb/) - 外部参考
