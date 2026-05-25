# Prompt: 理解 Cinux 项目

## 目的

让 AI 快速理解项目全貌，能够回答关于架构、模块、构建等方面的问题。

## 流程

### 第一步：读项目入口

- 读 `CLAUDE.md`（AI 主入口和工作流定义）
- 读 `document/todo/README.md`（路线图和 Feature 域划分）

### 第二步：浏览源码结构

按以下顺序浏览目录，理解模块组织：

1. `boot/` — 启动流程（BIOS → real mode → protected mode → long mode → kernel entry）
2. `kernel/arch/x86_64/` — 架构相关（GDT、IDT、上下文切换）
3. `kernel/mm/` — 内存管理（PMM、VMM、Heap、地址空间）
4. `kernel/proc/` — 进程管理（调度器、上下文、同步原语）
5. `kernel/drivers/` — 驱动（串口、VGA、键盘、AHCI、PCI、PIT）
6. `kernel/fs/` — 文件系统（VFS、ext2、ramdisk）
7. `kernel/gui/` — GUI（canvas、窗口管理器、位图、桌面）
8. `kernel/ipc/` — 进程间通信（pipe）
9. `kernel/syscall/` — 系统调用
10. `kernel/lib/` — 基础设施库（kprintf、string、atomic 等）

### 第三步：读代码规范

- 读 `document/ai_prompts/code_conventions.md`

### 第四步：读构建系统

- 读根 `CMakeLists.txt`，理解整体构建结构
- 读 `kernel/CMakeLists.txt`，理解内核构建
- 读 `test/CMakeLists.txt`，理解测试构建

## 约束

- **不猜测**，以源码为准
- 遇到不确定的，读对应源文件确认
- 项目使用 C++23 freestanding，禁用标准库、异常、RTTI
- 所有汇编使用 AT&T 语法
