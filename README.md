<div align="center">

# <img src="https://emojis.slackmojis.com/emojis/13825/blob-penguin.gif" width="40" alt="logo"> Cinux

### 从零手搓 x86_64 操作系统 · 中文教程 · 现代 C++ 实现

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![GCC](https://img.shields.io/badge/GCC-11.0%2B-blue)]()
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)]()
[![QEMU](https://img.shields.io/badge/QEMU-8.0%2B-orange)]()

一个"手把手"教你从 MBR 开始写操作系统的教程项目

</div>

---

## ✨ 项目简介

**Cinux** 是一个从零开始的 x86_64 操作系统开发教程，采用现代 C++23 编写，完全不依赖标准库。

> 💡 **为什么叫 Cinux？** = **C**in (**Ch**inese) + **Lin**ux-like，寓意"中文版 Linux 教程"

与大多数 OS 教程不同，Cinux 强调：

- 🎓 **三轨教程**：动手版（跟着敲）+ 通读版（看完整代码）+ 发布教程
- 🧪 **测试驱动**：轻量测试框架，Host 端 mock + QEMU 集成测试
- 🏗️ **现代工具链**：CMake + GNU 工具链，开箱即用
- 📖 **中文文档**：全中文讲解，降低学习门槛

---

## 🎯 你将学到什么

完成整个教程后，你将深入理解：

| 阶段 | 核心内容 | 关键技术 |
|:---:|:---------|:---------|
| 🔧 **Phase 1** | Bootloader | 实模式 → 保护模式 → 长模式，ELF 加载 |
| 🏗️ **Phase 2** | 内核基础设施 | GDT/IDT，中断处理，串口驱动 |
| 🖥️ **Phase 3** | 驱动三件套 | VGA 文本模式，PS/2 键盘，PIT 时钟 |
| 🧠 **Phase 4** | 内存管理 | 物理页分配，虚拟内存，内核堆 |
| ⚙️ **Phase 5** | 进程与调度 | 上下文切换，Round-Robin 调度器 |
| 👤 **Phase 6** | 用户态与系统调用 | Ring 3，syscall，Shell |
| 💾 **Phase 7** | 文件系统 | AHCI 驱动，VFS，Ext2 支持 |
| 🖼️ **Phase 8** | GUI（长期目标） | 窗口管理器，图形化应用 |

---

## 🚀 快速开始

### 前置要求

```bash
# Ubuntu/Debian
sudo apt install -y gcc g++ binutils qemu-system-x86_64 cmake

# 验证工具链
./scripts/check_toolchain.sh
```

### 三步启动

```bash
# 1️⃣ 配置构建
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 2️⃣ 编译并运行测试
make test

# 3️⃣ 启动 QEMU
make run
```

就这么简单！🎉

### 调试模式

```bash
# 终端 1：启动 QEMU + GDB server
make run-debug

# 终端 2：连接 GDB
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

---

## 🛠️ 技术栈亮点

<details>
<summary><b>🔍 现代C++ 内核开发（点击展开）</b></summary>

- ✅ **C++23 特性**：`constexpr`/`concepts`/`requires`/`modules`（计划）
- ✅ **编译期魔法**：GDT/IDT 描述符编译期生成
- ✅ **类型安全**：Concepts 约束驱动接口
- ✅ **RAII 资源管理**：锁、内存自动管理

</details>

<details>
<summary><b>🧪 自研测试框架（点击展开）</b></summary>

```cpp
// 极简 API
TEST("测试名称") {
    ASSERT_EQ(actual, expected);
    ASSERT_TRUE(condition);
}

// Host 端 mock 测试
#ifdef CINUX_HOST_TEST
    // 使用 mock 实现
#else
    // 真实硬件代码
#endif
```

</details>

<details>
<summary><b>📁 Git Tag 规范（点击展开）</b></summary>

每个 Milestone 完成后打 tag：`编号_大主题_小阶段`

```
000_env_toolchain     → 环境搭建完成
001_boot_real_mode    → 实模式启动完成
005_kernel_entry      → 内核入口完成
019_syscall           → 系统调用完成
```

</details>

---

## 📊 开发进度

<details>
<summary><b>Phase 1 · Bootloader</b></summary>

```
[████████████████████████████████████████████████████] 100%
✅ 001_boot_real_mode    实模式启动 + 磁盘读取
✅ 002_boot_gdt_protected 保护模式 + 串口驱动
✅ 003_boot_long_mode    长模式 + 页表初始化
✅ 004_boot_load_kernel  ELF 加载 + BootInfo
```

</details>

<details>
<summary><b>Phase 2 · 内核基础设施</b></summary>

```
[████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 25%
✅ 005_kernel_entry      内核入口 + kprintf
🔄 006_kernel_gdt_idt    GDT/IDT + 异常处理（进行中）
⬜ 007_pic_irq           PIC 重映射 + PIT 驱动
```

</details>

<details>
<summary><b>Phase 3 · 驱动三件套</b></summary>

```
[░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   0%
⬜ 008_driver_serial     串口驱动完善
⬜ 009_driver_vga_fb     VGA 文本模式 + PSF2 字体
⬜ 010_driver_keyboard   PS/2 键盘驱动
```

</details>

<details>
<summary><b>Phase 4-8 · 更多内容...</b></summary>

```
Phase 4: 内存管理  [░░░░░] 0%   |  Phase 5: 进程调度  [░░░░░] 0%
Phase 6: 用户态    [░░░░░] 0%   |  Phase 7: 文件系统  [░░░░░] 0%
Phase 8: GUI       [░░░░░] 0%
```

</details>

---

## 📚 教程结构

```
docs/
├── hands-on/          📝 动手版教程（跟着敲代码）
│   ├── 000-env-toolchain.md       ✅ 环境搭建
│   ├── 001-boot-real-mode.md      🔨 实模式启动
│   └── ...
└── read-through/      📖 通读版教程（看完整代码）
    ├── 001-boot-real-mode/        📂 完整代码 + 注释
    └── ...
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
