# F6: VFS 与文件系统

> VFS 层增强 + 虚拟文件系统 + ext4 基础。
> FAT32 推到 F11（UEFI 启动时一起做）。

## 实现决策

| 类别 | 内容 |
|------|------|
| VFS 增强 | Dentry Cache + 符号/硬链接 + 文件锁 |
| 虚拟 FS | ProcFS + DevFS + tmpfs/ramfs |
| 磁盘 FS | ext4 基础（extent-based 分配） |
| FAT32 | 推到 F11 |

## Milestone 依赖

```
M1 VFS 增强（dentry + symlink + flock）
       ↓
M2 ProcFS
M3 DevFS ──→ M4 tmpfs/ramfs（共享基础）
                      ↓
               M5 ext4 基础
```

M1 是所有后续的前置。M2/M3 可并行。M4 依赖 M3 的内存文件系统基础。M5/M6 独立。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-vfs-enhance.md](00-vfs-enhance.md) | M1: Dentry Cache + 链接 + 文件锁 |
| [01-procfs.md](01-procfs.md) | M2: ProcFS |
| [02-devfs.md](02-devfs.md) | M3: DevFS |
| [03-tmpfs.md](03-tmpfs.md) | M4: tmpfs/ramfs |
| [04-ext4.md](04-ext4.md) | M5: ext4 基础 |
| [05-ext2-lib.md](05-ext2-lib.md) | M6: ext2 独立库 + Host 端测试 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| VFS 抽象 | `kernel/fs/vfs_filesystem.hpp` — FileSystem 基类 |
| Inode/Ops | `kernel/fs/inode.hpp` — Inode + InodeOps |
| FD Table | `kernel/fs/file.hpp` — File + FDTable |
| Mount | `kernel/fs/vfs_mount.hpp` — MountPoint |
| Path | `kernel/fs/path.hpp` — 路径解析 |
| ext2 | `kernel/fs/ext2.hpp` — 当前磁盘文件系统 |
| Ramdisk | `kernel/fs/ramdisk.hpp` — ustar initrd |

## 验收标准

每个 Milestone：
1. 新 FS 可 mount + 基本文件操作
2. 不破坏现有 ext2/ramdisk 功能
3. 每文件 ≤ 500 行
