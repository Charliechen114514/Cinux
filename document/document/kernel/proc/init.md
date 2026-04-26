# Init 线程

> 里程碑: `028e_init_thread`

## 功能概述

内核 init 线程，等价于 Linux PID 1。负责文件系统挂载和第一个用户进程 (shell) 的启动。将启动逻辑从 `kernel_main` 中分离，对齐 Linux init 线程模型。

## 职责 (`kernel/proc/init.cpp`)
1. 挂载文件系统 (ext2)
2. 启动第一个用户进程 (shell 或 GUI 模式下的桌面)
3. 作为孤儿进程的 reaper

## kernel_main 启动流程
1. 硬件初始化 (GDT, IDT, PIC, PIT, Serial...)
2. `Scheduler::init()`
3. 创建 boot task → `kernel_init_thread()`
4. `run_first()` — 启动调度器
5. boot task 进入 yield+hlt 空闲回退

## AHCI 全局实例
- `AHCI::instance()` / `set_instance()` — 单例模式
- `main.cpp` 初始化后 `set_instance()`，`init.cpp` 通过 `instance()` 访问

## 统一内存布局
- `kernel/arch/x86_64/memory_layout.hpp` 定义 KMEM_HEAP / MMIO / STACK / DMA / EXT2_DMA
- 所有区域通过 base + size 计算，新增区域只需插入一行

## 关键修复
- 修复 `launch_first_user` 绕过调度器的问题
- 使用 placement new 避免 `AddressSpace` 析构器 `__dso_handle` 链接错误
- 修复内核栈虚拟地址覆盖 AHCI MMIO 映射导致 ext2 挂载失败

## 源码位置
- `kernel/proc/init.cpp` — init 线程实现
- `kernel/arch/x86_64/memory_layout.hpp` — 内存布局常量
