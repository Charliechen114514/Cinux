# Phase 0-A: CMake 架构升级审查

> **决策确认**：Phase 0 三子阶段严格串行（A→B→C）。ext2 头文件也拆分。

## 目标
精简 CMake 架构，确保大文件分拆，每个文件 ≤500 行，方便调试和阅读。

## 现状分析

### 当前文件规模（超标文件）
| 文件 | 行数 | 状态 |
|------|------|------|
| kernel/fs/ext2.cpp | 1810 | 严重超标 → 拆分 |
| kernel/proc/process.cpp | 937 | 超标 → 拆分 |
| kernel/gui/window_manager.cpp | 455 | 接近上限，暂不动 |
| kernel/gui/terminal.cpp | 421 | 接近上限，暂不动 |

### 当前 CMake 结构 (9 个 CMakeLists.txt)
```
CMakeLists.txt (root)
boot/CMakeLists.txt
kernel/CMakeLists.txt          ← big_kernel_common (68 源文件) 过于庞大
kernel/gui/CMakeLists.txt
kernel/mini/CMakeLists.txt
kernel/mini/lib/CMakeLists.txt
kernel/mini/test/CMakeLists.txt
user/CMakeLists.txt
test/CMakeLists.txt            ← 866 行，48 个测试手动定义
```

## 任务清单

### T1: 大文件分拆 — ext2（优先级：紧急）

**ext2.cpp (1810行) → 拆分为 5 组 .hpp/.cpp 对**，头文件也拆分：

| 新文件 | 职责 |
|--------|------|
| ext2_common.hpp / ext2_common.cpp | 共享常量、类型定义、工具函数 |
| ext2_superblock.hpp / ext2_superblock.cpp | 超级块读写与校验 |
| ext2_inode.hpp / ext2_inode.cpp | inode 操作（读/写/分配/释放） |
| ext2_directory.hpp / ext2_directory.cpp | 目录项操作（查找/创建/删除） |
| ext2_block.hpp / ext2_block.cpp | 数据块管理（分配/释放/读写） |
| ext2_init.hpp / ext2_init.cpp | 初始化与挂载逻辑 |

保留 `ext2.hpp` 作为统一的 forward header（#include 所有子头文件），确保外部调用方无需改动。

- [ ] 分析 ext2.cpp 的依赖图，确定拆分边界
- [ ] 创建子头文件和源文件
- [ ] ext2_common 放共享的 struct/ext2 类声明
- [ ] 每个 .cpp ≤ 500 行
- [ ] 更新 kernel/fs/CMakeLists.txt
- [ ] 验证构建通过

### T2: 大文件分拆 — process

**process.cpp (937行) → 拆分为 3 组 .hpp/.cpp 对**，头文件也拆分：

| 新文件 | 职责 |
|--------|------|
| process.hpp / process.cpp | Process 核心数据结构与生命周期 |
| task_builder.hpp / task_builder.cpp | TaskBuilder 实现与验证逻辑 |
| process_syscall.hpp / process_syscall.cpp | 进程相关系统调用辅助（fork/exec/wait） |

- [ ] 分析 process.cpp 的函数分组
- [ ] 创建子头文件和源文件
- [ ] 每个 .cpp ≤ 500 行
- [ ] 更新 kernel/proc/CMakeLists.txt
- [ ] 验证构建通过

### T3: CMake 子模块化

kernel/CMakeLists.txt 中的 big_kernel_common (68 个源文件) → 按子模块拆分为独立 OBJECT 库：

```
kernel/
├── CMakeLists.txt          ← 聚合所有子 OBJECT 库
├── arch/CMakeLists.txt     → arch_kernel (OBJECT)
├── drivers/CMakeLists.txt  → drivers_kernel (OBJECT)
├── fs/CMakeLists.txt       → fs_kernel (OBJECT)
├── mm/CMakeLists.txt       → mm_kernel (OBJECT)
├── proc/CMakeLists.txt     → proc_kernel (OBJECT)
├── syscall/CMakeLists.txt  → syscall_kernel (OBJECT)
├── ipc/CMakeLists.txt      → ipc_kernel (OBJECT)
├── lib/CMakeLists.txt      → lib_kernel (OBJECT)
└── gui/CMakeLists.txt      → gui_kernel (OBJECT, 条件编译)
```

- [ ] 为每个子目录创建 CMakeLists.txt
- [ ] kernel/CMakeLists.txt 改为 add_subdirectory + 聚合
- [ ] 验证构建通过

### T4: test/CMakeLists.txt 精简

- [ ] 用函数封装测试注册：
  ```cmake
  function(add_cinux_test name)
    add_executable(test_${name} unit/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE ...)
    add_test(NAME ${name} COMMAND test_${name})
  endfunction()
  ```
- [ ] 将 866 行缩减到 ~100 行

### T5: 编译选项审查
- [ ] 统一 -Werror（CI 中启用）
- [ ] 审查各模块 -O2 设置
- [ ] 增加 AddressSanitizer 支持（host 测试用）
- [ ] linker script 硬编码地址变量化

### T6: 500 行硬上限 CI 检查
- [ ] 添加脚本检查：`find kernel/ -name "*.cpp" -exec wc -l {} \; | awk '$1 > 500'`
- [ ] kernel/test/ 下测试文件同样受约束
- [ ] .h/.hpp 建议 ≤300 行

## 产出物
- [ ] 更新后的 CMakeLists.txt 文件集合（9 → 17 个）
- [ ] 拆分后的 ext2 和 process 源文件
- [ ] 验证构建通过 (`cmake --build build/`)
- [ ] CI 行数检查脚本
