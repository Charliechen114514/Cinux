<div align="center">

# <img src="https://emojis.slackmojis.com/emojis/13825/blob-penguin.gif" width="40" alt="logo"> Cinux

### 从零手搓 x86_64 操作系统 · 中文教程 · 现代 C++ 实现

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![GCC](https://img.shields.io/badge/GCC-Ubuntu%2024.04%2B-blue)]()
[![QEMU](https://img.shields.io/badge/QEMU-8.0%2B-orange)]()

一个"手把手"教你从 MBR 开始写操作系统的教程项目——从 Bootloader 到 GUI 桌面，全链路完成。Ubuntu 24.04 默认编译器即可构建，无需额外安装 GCC。

> **Note:** 为了保持轻量，笔者出于架构考虑，后续新特性的开发可能不会出手把手的三层教程了，而是以开发笔记的方式，重新发布到一个专门的组织仓库：[Cinux](https://github.com/CinuxOS/Cinux)。
> 但是这并不意味着这个仓库就Archive了，任何我觉得很不错的 Feature 完全可以迁移和搬运的，都会按照独立的方式进行搬运，更新教程；教程中任何潜在的错误我仍然会坚持维护！欢迎提任何Issue，不管是疑惑还是错误，还是实现的bug需要fix! 仓库仍然会活跃开发！

</div>

---

## ✨ 项目简介

**Cinux** 是一个从零开始的 x86_64 操作系统开发教程，采用现代 C++ 编写。

> 💡 **为什么叫 Cinux？**
> - C/C++'s Linux, 也就是尝试重新再写一个基于C/C++的Linux
> - CharlieChen's *nux（逃）

---

## 🖼️ Screenshots

<p align="center">
  <img src="assets/README/static_gui.png" width="45%" alt="GUI Desktop">
  <img src="assets/README/static_cli.png" width="45%" alt="Shell">
</p>
<p align="center">
  <em>GUI 桌面环境（左） · CLI 终端环境（右）</em>
</p>

<p align="center">
  <img src="assets/README/run_example.gif" width="45%" alt="Boot to Shell">
  <img src="assets/README/multi_shell.gif" width="45%" alt="Multi Terminal">
</p>
<p align="center">
  <em>从启动到 Shell（左） · 多终端窗口（右）</em>
</p>

<p align="center">
  <img src="assets/README/parallel_work.gif" width="45%" alt="Parallel Work">
  <img src="assets/README/filesystem.gif" width="45%" alt="Filesystem">
</p>
<p align="center">
  <em>多终端并发执行（左） · Ext2 文件操作（右）</em>
</p>

---

## 🌟 特性亮点

<table>
<tr>
<td width="50%">

🧠 **完整 x86_64 内核**
Bootloader → Mini Kernel → Big Kernel → User Space → GUI 桌面，全链路打通

</td>
<td width="50%">

📁 **Ext2 文件系统读写**
VFS 抽象层 + AHCI SATA 驱动，支持 touch/mkdir/rm/cat/ls/cd/stat

</td>
</tr>
<tr>
<td>

🖥️ **GUI 桌面环境**
Canvas 双缓冲 + 窗口管理器 + PS/2 鼠标驱动，支持拖动 / Z-order / 桌面图标

</td>
<td>

⚡ **多进程 & 多终端**
fork/execve/CoW 页表复制 + Pipe IPC，每个终端绑定独立 shell 进程

</td>
</tr>
<tr>
<td>

👨‍💻 **Ring 3 用户态 Shell**
22 个系统调用，内置 echo / help / clear / ls / cat / cd / pwd / stat / mkdir / rm / touch

</td>
<td>

🧪 **测试驱动开发**
自研轻量测试框架，Host 端 mock 测试 + QEMU 集成内核测试双轨并行

</td>
</tr>
<tr>
<td>

🔧 **现代 C++17 实现**
`constexpr` 编译期生成 GDT/IDT / `concepts` 类型约束 / RAII 锁管理 / `enum class` 驱动接口 / 支持用户态内核态 SSE （故支持-O2 Release构建）

</td>
</tr>
</table>

---

## 🎯 你将学到什么

完成整个教程后，你将深入理解：

| 阶段 | 内容 | 关键技术 |
|:---:|:------|:---------|
| Phase 1 | Bootloader | 实模式 → 保护模式 → 长模式，ELF 加载，VESA 图形模式，E820 内存探测 |
| Phase 2 | 小内核（Bootstrap） | 串口 / kprintf，PMM，IDT / 异常处理，ATA PIO 磁盘，ELF 加载 |
| Phase 3 | 大内核基础设施 | GDT / IDT / 256 向量中断，PIC 重映射，PIT 时钟 |
| Phase 4 | 驱动三件套 | VGA Framebuffer + PSF2 字体，PS/2 键盘驱动，串口完善 |
| Phase 5 | 内存管理 | PMM bitmap，VMM 4 级页表，内核堆（first-fit + coalesce），独立地址空间 |
| Phase 6 | 进程与调度 | context_switch，Round-Robin 调度器，Spinlock / Mutex / Semaphore |
| Phase 7 | 用户态与系统调用 | Ring 3 切换，syscall / sysret，22 个系统调用，用户态 Shell |
| Phase 8 | 文件系统 | AHCI SATA，VFS 抽象，Ext2 读写 + 目录操作 + stat，ramdisk |
| Phase 9 | GUI 桌面环境 | Canvas 双缓冲，窗口管理器，PS/2 鼠标，拖动 / Z-order，桌面图标 |
| Phase 10 | 多进程与高级特性 | fork / execve / CoW / waitpid，Pipe IPC，多终端并发 |

---

## 🚀 快速开始

### 前置要求

本项目支持最新的 g++ 15.2编译，使用CMake构建项目，您需要安装的是

```bash
# Ubuntu/Debian
sudo apt install -y gcc g++ binutils qemu-system-x86_64 cmake
```

### 构建 & 运行

🚀🚀🚀 在WSL 或者任何您喜欢的发行版中跑起来它们！🚀🚀🚀

> Feature Help: 不知道有没有好心人愿意移植到Windows上可编译，如果有所变动欢迎提交您的PR！

#### Step 1️⃣: 配置

```bash
#  配置为GUI（默认）（Release 模式），也是最推介的！🚀
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .

# 或者，默认（速度稍慢）
cmake -B build  -S .

# 或者你fork改炸了准备使用VSCode调试
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .

# 带测试的配置
cmake -B build -DCINUX_BUILD_TESTS=ON -S .

# CLI运行环境
cmake -B build -DCINUX_GUI=OFF -S .   
```

#### Step 2️⃣: 构建
```bash
cmake --build build -j$(nproc)
```

#### Step 3️⃣: Cinux，启动!

```bash
cmake --build build --target run # 跑内核本体, 默认Launch的是VNC显示，您需要VNC！
cmake --build build --target test_host           # Host 端单元测试（CTest）
cmake --build build --target run-kernel-test     # QEMU 内核测试（自动退出）
```

### 调试模式 1：GDB大牛请走这里

```bash
# 终端 1：启动 QEMU + GDB server
make run-debug

# 终端 2：连接 GDB
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

### 调试模式 2：VSCode大牛请走这里（是的别坐牢，如果不喜欢GDB!）

**Step 1：** 一键脚本构建并启动 QEMU 调试模式（Debug 构建 + GDB stub 监听 `:1234`）：

```bash
bash scripts/launch_qemu_debug.sh
```

**Step 2：** 确认 `.vscode/launch.json` 中已有如下配置：

> PS：大内核需要改一下ELF，这个麻烦自己手调。
```json
{
    "name": "QEMU 调试 (mini kernel)",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/kernel/mini/mini_kernel",
    "MIMode": "gdb",
    "miDebuggerServerAddress": "localhost:1234",
    ...
}
```

**Step 3：** 在 VSCode 中按 **F5**，选择对应的调试配置即可开始图形化断点调试。

---

## 🛠️ 技术栈亮点

<details>
<summary><b>🔍 现代 C++ 内核开发</b></summary>

- ✅ **C++17 特性**：`constexpr` / `concepts` / `requires`
- ✅ **编译期魔法**：GDT/IDT 描述符 `constexpr` 生成，桌面图标 `constexpr` 像素数据
- ✅ **类型安全**：`enum class` 作为 API 一等公民，`concepts` 约束驱动接口
- ✅ **RAII 资源管理**：Spinlock::guard、InterruptGuard、锁自动释放
- ✅ **零标准库依赖**： freestanding，自实现 memset/memcpy/string

</details>

<details>
<summary><b>🧪 自研测试框架</b></summary>

```cpp
// 极简 API
TEST("测试名称") {
    ASSERT_EQ(actual, expected);
    ASSERT_TRUE(condition);
}

// 双轨测试策略
// Host 端：mock 硬件，验证逻辑正确性（快速迭代）
// Kernel 端：QEMU 运行，验证真实硬件行为（端到端）
```

</details>

<details>
<summary><b>📁 42 个 Git Tags 覆盖全旅程</b></summary>

每个 Milestone 完成后打 tag：`编号_大主题_小阶段`

```
000_env_toolchain          → 环境搭建
001_boot_real_mode         → 实模式启动 + VESA 图形
009_large_kernel_entry     → 大内核入口
022_ring3_usermode         → Ring 3 用户态
028_fs_ext2                → Ext2 文件系统
033_gui_desktop            → GUI 桌面环境
035_multi_terminal         → 多终端并发
...
```

共 42 个 tag，覆盖从环境搭建到多终端的完整开发历程。

</details>

---

## 📊 开发进度

<details>
<summary><b>Phase 1 · Bootloader — 100% ✅</b></summary>

```
✅ 000_env_toolchain         环境搭建 + 工具链
✅ 001_boot_real_mode        实模式启动 + VESA 图形模式
✅ 002_boot_gdt_protected    保护模式 + 串口驱动
✅ 003_boot_long_mode        长模式 + 页表初始化
✅ 004_boot_load_mini_kernel ELF 加载 + BootInfo（A/B/C 三个子阶段）
```

</details>

<details>
<summary><b>Phase 2 · 小内核（Bootstrap Kernel）— 100% ✅</b></summary>

```
✅ 005_mini_kernel_entry     内核入口 + kprintf
✅ 006_mini_kernel_pmm       物理内存分配器
✅ 007_mini_kernel_intr      IDT + 异常处理
✅ 008_load_large_kernel     ATA 磁盘驱动 + ELF 加载器
```

</details>

<details>
<summary><b>Phase 3 · 大内核基础设施 — 100% ✅</b></summary>

```
✅ 009_large_kernel_entry    大内核入口 + 串口 + kprintf + 测试框架
✅ 010_big_kernel_gdt_idt    GDT/IDT + 256 向量中断 + 寄存器 dump
✅ 011_big_kernel_pic_irq    PIC 重映射 + PIT 时钟 + tick 计数
```

</details>

<details>
<summary><b>Phase 4 · 驱动三件套 — 100% ✅</b></summary>

```
✅ 012_driver_serial         串口驱动完善 + kprintf 格式化补全
✅ 013_driver_vga_fb         VGA Framebuffer + PSF2 字体 + Console
✅ 014_driver_keyboard       PS/2 键盘驱动 + 扫描码 + 环形队列
```

</details>

<details>
<summary><b>Phase 5 · 内存管理 — 100% ✅</b></summary>

```
✅ 015_mm_pmm                Bitmap 物理内存分配器
✅ 016_mm_vmm                4 级页表虚拟内存管理
✅ 017_mm_heap               内核堆 + kmalloc/kfree + new/delete
✅ 018_mm_address_space      独立地址空间 + 用户区隔离
```

</details>

<details>
<summary><b>Phase 6 · 进程与调度 — 100% ✅</b></summary>

```
✅ 019_proc_context          上下文切换 + 内核线程
✅ 020_proc_scheduler        Round-Robin 调度器 + idle task
✅ 021_proc_sync             Spinlock / Mutex / Semaphore
```

</details>

<details>
<summary><b>Phase 7 · 用户态与系统调用 — 100% ✅</b></summary>

```
✅ 022_ring3_usermode        Ring 3 切换 + TSS + sysret
✅ 023_syscall               syscall/sysret + 22 个系统调用
✅ 024_shell                 用户态 Shell（echo/help/clear）
```

</details>

<details>
<summary><b>Phase 8 · 文件系统 — 100% ✅</b></summary>

```
✅ 025_driver_ahci           AHCI SATA 驱动 + PCI 枚举
✅ 026_fs_ramdisk            ustar ramdisk 解析
✅ 027_fs_vfs                VFS 抽象层 + FDTable + ramdisk 文件系统
✅ 028_fs_ext2               Ext2 只读挂载
✅ 028b_fs_ext2_write        Ext2 写入 + touch/mkdir/rm/echo >
✅ 028c_fs_cwd_stat          cd/pwd/stat 系统调用
✅ 028d_sync_safety          全局并发安全审计 + 加锁
✅ 028e_activate_init_thread init 线程重构 + 统一虚拟内存布局
```

</details>

<details>
<summary><b>Phase 9 · GUI 桌面环境 — 100% ✅</b></summary>

```
✅ 029_gui_canvas            Canvas 双缓冲 + Bresenham 画线
✅ 030_gui_wm_basic          窗口管理器 + PS/2 鼠标 + 拖动
✅ 031_gui_native_app        Pipe IPC + Terminal 窗口 + Shell 集成
✅ 032_gui_bitmap_icon       32×32 位图图标 + constexpr 像素数据
✅ 033_gui_desktop           桌面背景 + 可点击图标 + 延迟创建终端
```

</details>

<details>
<summary><b>Phase 10 · 多进程与高级特性 — 100% ✅</b></summary>

```
✅ 034_process_fork_exec     fork/execve/CoW/waitpid + PID 管理
✅ 035_multi_terminal        每终端独立 shell 进程 + 多终端并发
```

</details>

---

## 📚 教程结构

```
document/
├── hands-on/          📝 22 篇动手教程（跟着敲代码）
│   ├── 000-env-toolchain.md           ✅ 环境搭建
│   ├── 001-boot-real-mode.md          🔨 实模式启动
│   ├── 005-mini-kernel-entry.md       🔨 小内核入口
│   ├── 009A-big-kernel-boot.md        🔨 大内核启动
│   └── ...
├── read-through/      📖 通读版教程（完整代码 + 注释）
├── tutorial/          📚 教学材料
└── notes/             📋 调试笔记 & 开发记录
```

---

## 🤝 参与贡献

欢迎贡献！你可以：

- 🐛 修复 Bug
- ✍️ 完善文档
- 💡 提出改进建议
- 📢 分享你的学习经验

---

## 📄 许可证

本项目采用 [MIT License](LICENSE) 开源协议。

---

## 🙏 致谢

- [OSDev Wiki](https://wiki.osdev.org/) - 宝贵的 OS 开发知识库
- [Writing an OS in Rust](https://os.phil-opp.com/) - 优秀的 OS 教程参考
- 所有为开源社区贡献的开发者

---

<div align="center">

**⭐ 如果这个项目对你有帮助，请给一个 Star！**

Made with ❤️ by [CharlieChen114514](https://github.com/Charliechen114514)

</div>
