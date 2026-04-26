# VFS & Ramdisk

> 里程碑: `026_fs_ramdisk` `027_fs_vfs`

## 功能概述

虚拟文件系统 (VFS) 抽象层，支持多后端挂载。第一个后端为 ustar 格式的 initrd ramdisk。提供 open/read/write/close/getdents 系统调用框架。

## VFS 核心 (`kernel/fs/vfs_filesystem.hpp`, `kernel/fs/vfs_mount.hpp/cpp`)

### FileSystem 抽象基类
```cpp
class FileSystem {
public:
    virtual ~FileSystem();
    virtual bool mount(const char* path) = 0;
    virtual Inode* lookup(const char* path) = 0;
};
```

### 挂载点管理
- `MountPoint` 结构: 文件系统路径 → FileSystem*
- `vfs_mount_init()` / `vfs_mount_add(path, fs)` / `vfs_mount_remove(path)`
- `vfs_mount_resolve(path)` — 最长前缀匹配

## Inode 层 (`kernel/fs/inode.hpp`)

### InodeOps — 函数指针表
```cpp
struct InodeOps {
    int (*read)(Inode*, void*, size_t, size_t*);
    int (*write)(Inode*, const void*, size_t, size_t*);
    int (*readdir)(Inode*, uint32_t, void*, size_t*);
    int (*create)(Inode*, const char*);
    int (*mkdir)(Inode*, const char*);
    int (*unlink)(Inode*, const char*);
    int (*stat)(Inode*, struct stat*);
};
```

### Inode 结构
- `ino`, `size`, `type` (InodeType 枚举), `ops`, `fs_private`, `mode`, `uid`, `gid`, 时间戳

## 文件描述符 (`kernel/fs/file.hpp/cpp`)
- `File` 结构: Inode 指针 + offset + flags
- `FDTable` 类: `alloc()` (从 fd 3 开始, 0-2 保留 stdin/stdout/stderr)
- `close(fd)` / `get(fd)` / `set(fd, file)` (用于 pipe)
- 线程安全: Spinlock 保护

## Ramdisk 后端 (`kernel/fs/ramdisk.hpp/cpp`)

### ustar 格式
```cpp
struct UstarHeader [[gnu::packed]] {
    char name[100], mode[8], uid[8], gid[8], size[12];
    char mtime[12], checksum[8], typeflag, magic[6];
};
```
- `ramdisk_mount(void* base)`: 遍历 512 字节对齐的 ustar 条目
- `ramdisk_lookup(path)`: 查找文件
- 实现 `FileSystem` 接口: `mount()` / `lookup()`

### CMake 集成
- initrd 归档嵌入内核镜像，通过 `_binary_initrd_start/end` 访问

## 系统调用
| 编号 | 功能 |
|------|------|
| `SYS_open` | resolve→lookup→alloc fd |
| `SYS_close` | FDTable close |
| `SYS_read` | fd=0 键盘, fd=1 kprintf, 其他走 VFS |
| `SYS_write` | fd=1 kprintf, 其他走 VFS |
| `SYS_getdents` | fd→Inode→ops→readdir |

## 安全
- 所有用户指针参数做 canonical address 校验

## 源码位置
- `kernel/fs/inode.hpp` — Inode 定义
- `kernel/fs/file.hpp/cpp` — FDTable
- `kernel/fs/vfs_filesystem.hpp` — FileSystem 基类
- `kernel/fs/vfs_mount.hpp/cpp` — 挂载点管理
- `kernel/fs/ramdisk.hpp/cpp` — ramdisk 后端
- `kernel/fs/ramdisk_config.hpp` — ramdisk 配置
