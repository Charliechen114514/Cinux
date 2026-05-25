---
title: 调试工作流
---

# 005-11: 调试工作流总结

## 概述

Cinux 项目提供多种调试方式，从简单的串口输出到完整的 VSCode 图形化调试。本文档总结各种调试模式的适用场景和工作流程。

---

## 1. 调试模式对比

| 模式 | 复杂度 | 交互性 | 适用场景 | 工具 |
|------|--------|--------|----------|------|
| 串口输出 | ★☆☆☆☆ | 无（单向） | 快速验证、日志输出 | kprintf, terminal |
| DebugConsole | ★☆☆☆☆ | 无（单向） | 早期启动调试 | kdebugf, debug.log |
| GDB 命令行 | ★★★☆☆ | 中（断点/单步） | 精确定位问题 | gdb, qemu -s -S |
| VSCode 调试 | ★★★★☆ | 高（图形界面） | 常规开发调试 | VSCode + GDB |

---

## 2. 快速参考

### 调试命令速查表

| 目标 | 命令 |
|------|------|
| 正常运行 | `make run` 或 `cd build && make run` |
| 调试模式 | `make run-debug` |
| VSCode 调试 | 按 `F5` |
| 查看串口日志 | `make run`（自动输出到终端） |
| 查看 debugcon | 检查 `build/debug.log` |

### VSCode 快捷键

| 快捷键 | 功能 |
|--------|------|
| `F5` | 启动调试 |
| `F10` | 单步跳过 |
| `F11` | 单步进入 |
| `Shift+F11` | 跳出函数 |
| `Shift+F5` | 停止调试 |

---

## 3. 典型调试场景

### 场景 1：内核不启动

**症状**：QEMU 启动后无任何输出

**调试流程**：

```bash
# 1. 检查 debugcon 输出
cat build/debug.log
# 应该看到 "1234" 表示启动各阶段完成

# 2. 使用 QEMU 调试模式
cd build && make run-debug
# 另一终端启动 gdb
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
(gdb) x/5i $pc
```

### 场景 2：变量值异常

**症状**：某个变量显示错误值

**调试流程**：

```bash
# 1. 使用 VSCode 图形调试
# 打开 main_test.cpp，在变量所在行设置断点
# 按 F5 启动调试

# 2. 查看变量
# 在 WATCH 窗口添加变量名
# 或在 DEBUG CONSOLE 输入:
-exec print variable_name

# 3. 查看内存
-exec x/10gx &variable_name
```

### 场景 3：格式化输出错误

**症状**：kprintf 输出格式不对

**调试流程**：

```bash
# 1. 运行 Host 测试（直接在 Linux 上测试格式化）
make test_host

# 2. 如果通过，说明格式化算法正确
# 问题可能在串口发送

# 3. 在串口驱动设置断点
gdb build/kernel/mini/mini_kernel
(gdb) break Serial::putc
(gdb) continue
```

### 场景 4：C++ 运行时问题

**症状**：虚函数调用失败、构造函数未执行

**调试流程**：

```bash
# 1. 运行 C++ 运行时测试
cd build && make run-kernel-test

# 2. 在测试函数设置断点
gdb build/kernel/mini/mini_kernel_test
(gdb) break test1::test_simple_class
(gdb) break test2::test_virtual_functions
(gdb) continue
```

---

## 4. 完整调试工作流

### VSCode 图形化调试（推荐）

```
┌─────────────────────────────────────────────────────────────┐
│                 VSCode 调试工作流                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. 打开项目                                                │
│     code /path/to/cinux                                    │
│                                                             │
│  2. 打开源文件                                              │
│     kernel/mini/test/main_test.cpp                         │
│                                                             │
│  3. 设置断点                                                │
│     点击行号左侧                                            │
│                                                             │
│  4. 启动调试                                                │
│     按 F5                                                  │
│     或选择 "Run and Debug" → "QEMU 调试 (mini kernel)"    │
│                                                             │
│  5. 调试操作                                                │
│     - F10: 单步跳过                                         │
│     - F11: 单步进入                                         │
│     - Shift+F11: 跳出函数                                   │
│     - 鼠标悬停查看变量                                      │
│     - WATCH 窗口添加表达式                                  │
│     - CALL STACK 查看调用栈                                 │
│                                                             │
│  6. 查看串口输出                                            │
│     输出会显示在 VSCode 终端（QEMU 任务）                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### GDB 命令行调试

```bash
# Terminal 1: 启动 QEMU
cd build && make run-debug

# Terminal 2: 启动 GDB
gdb build/kernel/mini/mini_kernel

# GDB 会话
(gdb) target remote :1234       # 连接 QEMU
(gdb) break mini_kernel_main    # 设置断点
(gdb) continue                  # 继续执行
(gdb) print __boot_info_ptr     # 打印变量
(gdb) x/10i $pc                 # 反汇编当前指令
(gdb) step                      # 单步执行
(gdb) bt                        # 查看调用栈
(gdb) quit                      # 退出
```

### 串口日志调试

```bash
# 运行内核，输出显示在终端
cd build && make run

# 或将串口输出保存到文件
qemu-system-x86_64 \
    -drive file=cinux.img,format=raw \
    -serial file:serial.log \
    -no-reboot -no-shutdown

# 查看日志
cat serial.log
```

---

## 5. 调试技巧

### 技巧 1：条件断点

```gdb
# 只在特定条件下暂停
(gdb) break main if argc == 0
(gdb) print ptr if ptr != nullptr
```

### 技巧 2：观察点

```gdb
# 当内存/变量值变化时触发
(gdb) watch *0xaddress
(gdb) watch global_variable
```

### 技巧 3：反汇编当前函数

```gdb
(gdb) disassemble
# 或
(gdb) x/20i $pc
```

### 技巧 4：查看调用栈

```gdb
(gdb) bt
# 切换栈帧
(gdb) frame 2
# 查看局部变量
(gdb) info locals
```

### 技巧 5：双输出调试

```cpp
// 同时输出到串口和 debugcon
kprintf("Serial: %s\n", msg);
kdebugf("Debug: %s\n", msg);
```

---

## 6. 故障排查决策树

```
问题出现
    │
    ├─► 有输出但行为异常？
    │   │
    │   └─► 使用 GDB/VSCode 跟踪执行流程
    │       设置断点 → 单步执行 → 检查变量
    │
    ├─► 完全无输出？
    │   │
    │   ├─► 检查 debugcon
    │   │   cat build/debug.log
    │   │
    │   └─► 使用 QEMU 调试模式
    │       make run-debug
    │       gdb → target remote :1234 → break _start
    │
    └─► 编译错误？
        │
        └─► 检查构建配置
            cmake -DCMAKE_BUILD_TYPE=Debug -B build
            make -C build clean
            make -C build
```

---

## 7. 性能考虑

| 调试方式 | 开销 | 建议 |
|----------|------|------|
| kprintf（串口） | 中（轮询等待） | 限制高频调用 |
| kdebugf（debugcon） | 低（直接写入） | 可频繁使用 |
| GDB 断点 | 低（硬件支持） | 精确调试 |
| GDB 单步 | 极高（每步暂停） | 仅必要时使用 |

---

## 8. 相关链接

### 内部文档
- [005-06: kprintf 格式化实现](005-06-kprintf-format.md) - 串口输出接口
- [005-07: 格式化算法详解](005-07-format-algorithms.md) - 格式化算法
- [005-08: VSCode 调试配置](005-08-vscode-debug-setup.md) - VSCode 配置详解
- [005-09: QEMU GDB 调试](005-09-qemu-gdb-debug.md) - GDB 命令参考
- [005-10: 串口调试详解](005-10-serial-debug.md) - UART 驱动

### 外部资源
- [QEMU GDB documentation](https://www.qemu.org/docs/master/system/gdb.html)
- [Building a VS Code Debugging Workflow for a Custom x64 OS](https://www.sqlpassion.at/archive/2025/07/22/building-a-vs-code-debugging-workflow-for-a-custom-x64-operating-system-with-qemu-and-gdb/)
- [OSDev Wiki: GDB](https://wiki.osdev.org/GDB)

---

## 9. 文件清单

| 文件 | 用途 |
|------|------|
| [`.vscode/launch.json`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/launch.json) | VSCode 调试配置 |
| [`.vscode/tasks.json`](https://github.com/CinuxOS/Cinux/blob/main/.vscode/tasks.json) | 构建任务 |
| [`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake) | QEMU 参数 |
| [`kernel/mini/lib/kprintf.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/kprintf.cpp) | 调试输出 |
| [`kernel/mini/driver/serial.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/serial.cpp) | 串口驱动 |
