# 文件描述符 & 路径解析

> 里程碑: `027_fs_vfs` `028c_fs_cwd_stat`

## 功能概述

per-process 文件描述符管理、绝对/相对路径解析、stat 元数据结构。

## 文件描述符 (`kernel/fs/file.hpp/cpp`)
- `OpenFlags` 枚举
- `File` 结构: Inode 指针 + offset + flags
- `FDTable` 类:
  - `alloc()` — 从 fd 3 开始 (0=stdin, 1=stdout, 2=stderr 保留)
  - `close(fd)` — 关闭并释放 slot
  - `get(fd)` — 获取 File
  - `set(fd, file)` — 手动设置 (用于 pipe fd 绑定)
- 线程安全: Spinlock 保护 (TIER 0)

## 路径解析 (`kernel/fs/path.hpp/cpp`)
- 绝对路径: 直接 VFS resolve
- 相对路径: 结合进程 cwd 拼接后再 resolve
- 路径规范化: 处理 `.` / `..` 等

## stat 结构 (`kernel/fs/stat.hpp`)
```cpp
struct stat {
    uint32_t st_mode;
    uint64_t st_size;
    uint32_t st_ino;
    uint32_t st_type;
};
```

## 源码位置
- `kernel/fs/file.hpp/cpp` — File & FDTable
- `kernel/fs/path.hpp/cpp` — 路径解析
- `kernel/fs/stat.hpp` — stat 结构
