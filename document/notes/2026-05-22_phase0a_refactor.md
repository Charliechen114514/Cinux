---
title: Phase 0-A 重构日志
---

# Phase 0-A 重构开发日志 (2026-05-22)

> 分支：`optimize/optimize_tutorials_for_release`
> 提交：`145b488 optimize: update the code quality`

## 目的

Phase 0 是所有 Feature 域开发的前置阶段，分为 0-A（CMake 架构升级 + 大文件拆分）、0-B（代码优化）、0-C（注释覆盖）三个串行子阶段。本次完成 0-A。

核心问题：两个巨型文件（ext2.cpp 1810 行、process.cpp 937 行）严重超标 500 行硬上限，kernel/CMakeLists.txt 把 68 个源文件堆在一个 OBJECT 库里，test/CMakeLists.txt 有 866 行重复样板代码。这些在开发后期会造成维护困难、编译时间增长、CI 无法自动检查行数合规。

## 重构手段

### 1. ext2.cpp 拆分（1810 行 → 5 个文件）

按照逻辑功能点拆分，每个文件保持 ≤500 行：

| 文件 | 行数 | 职责 |
|------|------|------|
| ext2_common.cpp | ~320 | Ext2FileOps / Ext2DirOps 的 VFS InodeOps 桥接实现 |
| ext2_init.cpp | ~310 | 构造函数、DMA buffer、mount()、块 I/O、超级块/BGDT 写回、路径解析 |
| ext2_inode.cpp | ~395 | inode 读写、分配/释放、缓存管理、get_or_alloc_block() |
| ext2_block.cpp | ~155 | 数据块 bitmap 扫描、分配/释放、元数据计数更新 |
| ext2_directory.cpp | ~470 | 目录项插入/删除、create()、mkdir()、unlink() |

头文件也做了相应拆分：
- `ext2_common.hpp` 提取了 Ext2FileOps、Ext2DirOps 类定义和 EXT2_MAX_GROUPS 常量，避免 ext2.hpp 膨胀
- `ext2.hpp` 保留为统一入口，include ext2_common.hpp + ext2_types.hpp
- `ext2_types.hpp` 不变（磁盘结构体和常量）
- 外部调用方（proc/init.cpp、测试文件等）无需任何改动

拆分原则：所有方法仍属于 Ext2 类，只是实现分散到多个 .cpp 文件。这是 C++ 的标准做法——类定义在头文件，实现可以分布在多个翻译单元。函数之间的调用关系决定了归属：例如 lookup_in_dir/lookup 放在 ext2_init.cpp 因为它们是 mount 后的路径导航基础设施，而 create/mkdir/unlink 放在 ext2_directory.cpp 因为它们是目录变更操作。

### 2. process.cpp 拆分（937 行 → 4 个文件）

原计划 3 个文件，但 fork + execve 的内部辅助函数合计 ~760 行，超过 500 行限制，因此拆为 4 个：

| 文件 | 行数 | 职责 |
|------|------|------|
| process_new.cpp | ~260 | 共享内部状态（next_tid、栈地址分配器）、CoW page fault 处理、waitpid() |
| task_builder.cpp | ~160 | TaskBuilder setter 和 build() 实现 |
| fork.cpp | ~280 | fork() 实现 + CoW 页表递归拷贝 |
| execve.cpp | ~290 | execve() 实现 + ELF 校验 / 用户空间清空 |

共享状态处理：原代码中 `next_tid` 和 `alloc_stack_vaddr()` 在匿名命名空间里，拆分后需要跨文件共享。解决方案是创建 `process_internal.hpp`，声明 extern 变量和函数，实际定义在 process_new.cpp 中。

### 3. CMake 子模块化

kernel/CMakeLists.txt 从内联 68 个源文件改为 `add_subdirectory` 模式：

```
kernel/CMakeLists.txt          ← 创建空 OBJECT 库，add_subdirectory 聚合
kernel/arch/CMakeLists.txt     ← arch 源文件（boot.S, gdt.cpp, ...）
kernel/drivers/CMakeLists.txt  ← 驱动源文件（含 GUI 条件编译）
kernel/fs/CMakeLists.txt       ← 文件系统源文件
kernel/mm/CMakeLists.txt       ← 内存管理源文件
kernel/proc/CMakeLists.txt     ← 进程管理源文件
kernel/syscall/CMakeLists.txt  ← 系统调用源文件
kernel/ipc/CMakeLists.txt      ← IPC 源文件
kernel/lib/CMakeLists.txt      ← 基础库源文件
kernel/gui/CMakeLists.txt      ← GUI 源文件（已有，保留）
```

采用 `target_sources(big_kernel_common PRIVATE ...)` 模式而非独立 OBJECT 库，理由：
- 与已有 gui/CMakeLists.txt 保持一致
- 避免为每个子库重复设置编译选项
- 编译选项统一在父 CMakeLists.txt 设置一次

### 4. test/CMakeLists.txt 精简（866 行 → ~200 行）

引入两个辅助函数消除重复样板：

```cmake
function(add_cinux_test name)          # 简单测试：unit/test_<name>.cpp
function(add_cinux_integration_test name)  # 集成测试：自定义源文件列表
endfunction()
```

同时把 `test_host`、`test_all`、`test_verbose` 的 DEPENDS 列表提取为 `ALL_HOST_TESTS` 变量，一处维护。

### 5. CI 行数检查

新增 `scripts/check_line_limits.py`：扫描 kernel/（排除 mini/ 和 test/），.cpp/.S 文件限制 500 行，.h/.hpp 文件限制 500 行（建议 300 行）。CI 新增 `line-limits` job 在格式化后、构建前运行。

## 验证结果

- `cmake --build build/` 完整构建通过（内核 + host 测试）
- 全部 41 个 host 单元测试通过
- 全部 662 个 kernel (QEMU) 测试通过
- `python3 scripts/check_line_limits.py --hpp 500` 全部通过
- 所有 .cpp 文件 ≤ 500 行
