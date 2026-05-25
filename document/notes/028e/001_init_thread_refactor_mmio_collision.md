---
title: Init 线程重构
---

# 028e · 重构调度器启动为 Linux init 线程模型 — 排查笔记

## 背景

将 `Scheduler::init()` 从 `run_concurrent_stress()` 中提取出来，对齐 Linux 的 init 线程模型：
- boot CPU 上下文 → boot task（类似 PID 0 idle）
- spawn `kernel_init` 线程（类似 PID 1）负责文件系统挂载和 shell 启动
- 同时修复 `launch_first_user()` 中手动创建零初始化 `static Task shell_task{}` 的问题

## 变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `kernel/proc/init.hpp` | 新建 | 声明 `kernel_init_thread()` |
| `kernel/proc/init.cpp` | 新建 | init 线程：ext2 挂载 + VFS + launch_first_user |
| `kernel/arch/x86_64/memory_layout.hpp` | 新建 | 统一内核虚拟内存布局定义 |
| `kernel/main.cpp` | 修改 | Scheduler::init() 直接调用，spawn init 线程，删除 stress/键盘轮询 |
| `kernel/arch/x86_64/usermode.cpp` | 修改 | 删除 shell_task，使用调度器管理的当前 Task |
| `kernel/drivers/ahci/ahci.hpp` | 修改 | 添加 instance()/set_instance() 静态方法 |
| `kernel/drivers/ahci/ahci.cpp` | 修改 | 实现 instance/set_instance，MMIO_VIRT_BASE 引用统一布局 |
| `kernel/proc/process.cpp` | 修改 | next_stack_vaddr 引用统一布局 |
| `kernel/fs/ext2.cpp` | 修改 | EXT2_DMA_VIRT_BASE 引用统一布局 |
| `kernel/stress/stress_test.cpp` | 删除 | 整个 stress 目录移除 |
| `kernel/CMakeLists.txt` | 修改 | 添加 proc/init.cpp，删除 stress_test.cpp |

---

## 排查记录

### 问题 1：ext2 mount 失败 — "command timeout"

**现象**：重构后 ext2 挂载报 `Port 1: command timeout`，但 AHCI 初始化阶段 Port 1 检测到设备（`SSTS=0x113 DET=3`）。

**初步假设**：调度器抢占导致 AHCI DMA 轮询被打断，浪费了超时时间。

**验证**：在 ext2 挂载外加 `InterruptGuard` 关中断，问题依旧。排除抢占假设。

### 问题 2（根因）：内核栈虚拟地址覆盖 AHCI MMIO 映射

**关键发现**：在 init 线程中打印 Port 1 的 SSTS 寄存器：

```
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0xffffffff   ← AHCI init 时，设备在线
[INIT] Port 1 SSTS=0x0 before mount                ← init 线程中，设备消失！
```

Port 1 的设备状态从 `0x113` 变成了 `0x0`。MMIO 寄存器读出全零 = 页表映射被覆盖。

**根因分析**：

```
AHCI MMIO 虚拟基址 (ahci.cpp):  0xFFFF800000100000
内核栈虚拟起始  (process.cpp):  0xFFFF800000100000  ← 完全相同！
```

`TaskBuilder::build()` 为每个任务分配 4 页内核栈，通过 `g_vmm.map()` 映射到 `0xFFFF800000100000` 起始的虚拟地址。这覆盖了 AHCI BAR5 的 MMIO 页表项：

```
Scheduler::init() 创建 idle task → 栈映射到 0xFFFF800000100000 → 覆盖 AHCI MMIO
TaskBuilder 创建 kernel_init   → 栈映射到 0xFFFF800000104000 → 覆盖 AHCI cmdlist/fis
TaskBuilder 创建 boot task     → 栈映射到 0xFFFF800000108000 → 进一步覆盖
```

**为什么重构前没暴露**：重构前 `Scheduler::init()` 在 `run_concurrent_stress()` 中调用，AHCI init 和 ext2 mount 都在 `kernel_main()` 中（调度器启动前）完成，此时还没有内核栈映射，MMIO 区域完好。

**修复**：创建 `kernel/arch/x86_64/memory_layout.hpp`，统一管理内核虚拟内存布局：

```
KMEM_HEAP_BASE  = 0xFFFF800000000000  (1 MB)
KMEM_MMIO_BASE  = 0xFFFF800000100000  (256 KB)
KMEM_STACK_BASE = 0xFFFF800000150000  ← MMIO 之后
KMEM_DMA_BASE   = 0xFFFF800000250000
KMEM_EXT2_DMA_BASE = 0xFFFF800000350000
```

所有模块通过 `base + size` 计算地址，新增区域只需插入一行并调整后续基址。

### 问题 3：launch_first_user() 的 static Task 绕过调度器

**现象**：`usermode.cpp:167` 手动创建 `static Task shell_task{}`，零初始化、无内核栈、tid=0、不在运行队列。

**影响**：
- `sys_exit` 调用 `Scheduler::current()` 返回这个伪 Task，标记 Dead 后 yield，但该 Task 不在运行队列中
- 没有有效的 CpuContext，如果调度器尝试切回它会崩溃

**修复**：使用调度器管理的当前 Task（kernel_init 线程），只更新 `addr_space` 和 `cwd`。`user_space` 改为 placement new 在 static buffer 上（避免 `__dso_handle` 链接错误，因为 AddressSpace 有析构器）。

### 问题 4：AHCI 全局实例的跨翻译单元访问

**现象**：`init.cpp` 在 `big_kernel_common` 中，`g_kernel_ahci` 定义在 `main.cpp` 中。test binary 没有 `g_kernel_ahci` 定义导致链接失败。

**修复**：在 AHCI 类中添加 `static instance()/set_instance()` 方法，遵循现代 C++ 封装原则。`main.cpp` 初始化后调用 `set_instance()`，`init.cpp` 通过 `AHCI::instance()` 访问。
