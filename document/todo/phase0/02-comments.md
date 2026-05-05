# Phase 0-C: 注释优化审查

> **决策确认**：严格串行在 0-B 之后。只改注释不改代码。英文注释。
> 采用全套 CFDesktop Doxygen 规范（@brief/@param/@return/@throws/@note/@warning/@since/@ingroup 全部必填）。
> 参考 CFDesktop 的 scripts/doxygen/lint.py 做 linter 检查。

## Doxygen 注释规范（对齐 CFDesktop）

### 文件头模板
```cpp
/**
 * @file    relative/path/to/file.h
 * @brief   One-sentence summary.
 *
 * Longer description (optional, max 2-3 sentences).
 *
 * @author  <git:author-name or "N/A">
 * @date    <git:last-commit-date or "N/A">
 * @version <git:last-tag-or-commit or "N/A">
 * @since   <project version or "N/A">
 * @ingroup <module or "none">
 */
```

### 函数/方法模板
```cpp
/**
 * @brief  Short description in third-person present tense.
 *
 * Detailed description (optional, few short sentences).
 *
 * @param[in]   name    Description with units and valid range.
 * @param[out]  out     Description with ownership semantics.
 * @tparam T            Template parameter description.
 * @return             Return value description (omit for void).
 * @throws             Exceptions thrown, or @throws None.
 * @note               Clarifications or constraints.
 * @warning            Warnings (reentrancy, performance).
 * @since              Version or "N/A".
 * @ingroup            Module name or "none".
 */
```

### 语言规则
- MUST use English only
- MUST use third-person present tense ("Initializes", not "will initialize")
- MUST NOT use first-person ("we", "I", "our")
- MUST NOT use future tense ("will")

## 模块审查顺序

### C1: kernel/lib/ — 基础库
- [ ] kprintf.cpp — 格式化函数行为注释
- [ ] string.cpp — 每个字符串函数的行为和边界条件

### C2: kernel/mm/ — 内存管理
- [ ] pmm.cpp — 物理内存布局、bitmap 索引计算
- [ ] vmm.cpp — 虚拟地址空间布局、页表项格式
- [ ] heap.cpp — 堆块结构、分配/释放算法
- [ ] address_space.cpp — 地址空间切换的 TLB 影响

### C3: kernel/fs/ — 文件系统
- [ ] ext2_common.cpp — ext2 磁盘布局（超级块、块组描述符、inode 表）
- [ ] ext2_superblock.cpp — 超级块字段说明
- [ ] ext2_inode.cpp — inode 操作说明
- [ ] ext2_directory.cpp — 目录项格式
- [ ] ext2_block.cpp — 数据块管理
- [ ] ext2_init.cpp — 挂载流程
- [ ] ramdisk.cpp — ramdisk 内存布局
- [ ] vfs_mount.cpp — VFS 抽象设计
- [ ] path.cpp — 路径解析规则
- [ ] file.cpp — 文件描述符管理
- [ ] inode.cpp — inode 抽象层

### C4: kernel/drivers/ — 驱动层
- [ ] ahci/ahci.cpp — AHCI 寄存器布局和命令流程（参考 Intel AHCI Spec）
- [ ] keyboard/keyboard.cpp — PS/2 键盘扫描码集
- [ ] mouse.cpp — PS/2 鼠标协议
- [ ] pci/pci.cpp — PCI 配置空间布局（参考 PCI Spec）
- [ ] pit/pit.cpp — PIT 频率和定时器模式
- [ ] canvas.cpp — 绘图算法
- [ ] video/framebuffer.cpp — 帧缓冲映射
- [ ] video/console.cpp — 控制台滚动和光标管理
- [ ] video/font.cpp — PSF2 字体格式
- [ ] serial/serial.cpp — UART 16550 寄存器

### C5: kernel/arch/x86_64/ — 架构层（最高优先级，硬件约束多）
- [ ] boot.S — 每一阶段的状态转换（16-bit → 32-bit → 64-bit）
- [ ] interrupts.S — 中断栈帧布局
- [ ] syscall.S — syscall ABI (Syscall # → rax, args → rdi/rsi/rdx/r10/r8/r9)
- [ ] context_switch.S — 寄存器保存/恢复约定
- [ ] usermode.S — 用户态切换流程（iretq 栈布局）
- [ ] gdt.cpp — GDT entry 布局（参考 Intel SDM Vol.3A §3.5）
- [ ] idt.cpp — IDT entry 布局（参考 Intel SDM Vol.3A §6.10）
- [ ] pic.cpp — PIC 重映射原因（IRQ 0-15 → INT 32-47）
- [ ] exception_handlers.cpp — 异常分类（fault/trap/abort）
- [ ] irq_handlers.cpp — IRQ 与异常映射
- [ ] paging.cpp — 页表项格式（参考 Intel SDM Vol.3A §4.5）
- [ ] syscall.cpp — syscall number 与参数映射
- [ ] usermode.cpp — 用户态入口约定
- [ ] crt_stub.cpp — 构造函数数组 (.init_array)

### C6: kernel/proc/ — 进程管理
- [ ] process.cpp — 进程状态机和生命周期
- [ ] task_builder.cpp — TaskBuilder 构建步骤
- [ ] process_syscall.cpp — 进程 syscall 辅助函数
- [ ] scheduler.cpp — 调度算法和优先级
- [ ] sync.cpp — 同步原语实现策略
- [ ] init.cpp — init 进程启动流程
- [ ] pid.cpp — PID 分配策略
- [ ] elf_types.cpp — ELF 结构体映射

### C7: kernel/syscall/ — 系统调用
- [ ] 每个 syscall 文件 — 确认注释准确
- [ ] 标注 POSIX 兼容性差异
- [ ] 标注 errno 映射

### C8: kernel/ipc/ — 进程间通信
- [ ] pipe.cpp — 管道缓冲区管理和阻塞语义
- [ ] pipe_ops.cpp — 管道操作语义

### C9: kernel/gui/ — 图形界面
- [ ] window_manager.cpp — 窗口管理算法
- [ ] terminal.cpp — 终端仿真逻辑（VT100 escape sequence）
- [ ] gui_init.cpp — GUI 初始化流程
- [ ] window.cpp — 窗口属性管理
- [ ] event.cpp — 事件分发机制

## 自动化检查
- [ ] 移植 CFDesktop 的 scripts/doxygen/lint.py 到 tools/
- [ ] 适配 Cinux 的文件结构和命名规范
- [ ] CI 中运行 Doxygen linter

## 产出物
- [ ] 每个模块的注释更新 patch
- [ ] Doxygen linter 工具
- [ ] 注释准确性检查报告
