# 项目文档

内核架构设计、构建系统、测试框架等项目级文档。

## 内容结构

### 内核架构

| 文档 | 说明 |
|------|------|
| [内核总览](kernel/mini.md) | Mini 内核架构概览 |
| [架构设计](kernel/arch.md) | 模块划分与依赖关系 |
| [标准库](kernel/lib.md) | freestanding C++ 库设计 |
| [系统调用](kernel/syscall.md) | syscall 接口设计 |
| [测试框架](kernel/test.md) | 内核态测试基础设施 |

### 子系统

| 子系统 | 文档 |
|--------|------|
| 内存管理 | [PMM](kernel/mm/pmm.md)、[VMM](kernel/mm/vmm.md)、[地址空间](kernel/mm/address_space.md)、[堆](kernel/mm/heap.md) |
| 进程管理 | [进程与调度](kernel/proc/process_and_scheduler.md)、[PID](kernel/proc/pid.md)、[Fork/Exec](kernel/proc/fork_exec.md)、[Init](kernel/proc/init.md)、[同步](kernel/proc/sync.md) |
| 文件系统 | [VFS 与 Ramdisk](kernel/fs/vfs_and_ramdisk.md)、[Ext2](kernel/fs/ext2.md)、[文件与路径](kernel/fs/file_and_path.md) |
| 驱动 | [串口](kernel/drivers/serial.md)、[键盘](kernel/drivers/keyboard.md)、[鼠标](kernel/drivers/mouse.md)、[视频](kernel/drivers/video.md)、[PIT](kernel/drivers/pit.md)、[AHCI/PCI](kernel/drivers/ahci_pci.md)、[Canvas](kernel/drivers/canvas.md) |
| IPC | [Pipe](kernel/ipc/pipe.md) |
| GUI | [窗口管理](kernel/gui/wm_and_events.md)、[Canvas 与窗口](kernel/gui/canvas_and_window.md)、[桌面与图标](kernel/gui/desktop_and_icons.md)、[终端](kernel/gui/terminal.md) |

### 其他

| 文档 | 说明 |
|------|------|
| [启动流程](boot.md) | 从 MBR 到内核入口 |
| [构建系统](scripts.md) | Makefile 和构建脚本 |
| [测试](testing.md) | 测试策略和覆盖率 |
| [用户态](user/libc.md) | libc 接口和 Shell |

## 适合谁

- 想从架构层面理解 Cinux 的开发者
- 需要查找特定子系统设计的贡献者
- 准备扩展某个内核模块的人

## 推荐阅读

- 入门：从 [启动流程](boot.md) 和 [内核总览](kernel/mini.md) 开始
- 按子系统深入：在上方表格中找到感兴趣的模块
