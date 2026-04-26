# ext2 文件系统

> 里程碑: `028_fs_ext2` `028b_fs_ext2_write` `028c_fs_cwd_stat`

## 功能概述

ext2 文件系统驱动，挂载 QEMU ext2 分区。支持读取、写入、创建/删除文件和目录、per-process 工作目录和 stat 元数据。数据通过 AHCI SATA DMA 回写磁盘。

## 磁盘格式

### Superblock (`kernel/fs/ext2_types.hpp`)
- `s_inodes_count / s_blocks_count / s_log_block_size / s_magic = 0xEF53`
- `block_size = 1024 << s_log_block_size`

### Inode
- `i_mode / i_uid / i_size / i_block[15]`
- 直接块 0-11，`i_block[12]` 单重间接块，`i_block[13]` 双重间接块

### 目录项
- `Ext2DirEntry [[gnu::packed]]`: `inode / rec_len / name_len / file_type / name[]`

## 初始化 (`ext2_init()`)
1. 读 superblock (磁盘偏移 1024B)，验证 magic `0xEF53`
2. 计算 `block_size`
3. 读 block group descriptor table

## 读取操作
- `ext2_read_inode(ino)` — group=(ino-1)/inodes_per_group，偏移计算
- `ext2_read_file(inode, buf, offset, len)` — 遍历 `i_block[0-11]` + `i_block[12]` 间接块
- `ext2_readdir(inode, index)` — `rec_len` 步进遍历目录数据块

## 写入操作 (028b)
- **Block 分配器**: 扫描 block bitmap → 分配并标记 → 回写 bitmap
- **Inode 分配器**: 扫描 inode bitmap → 分配并标记 → 回写 bitmap
- `ext2_file_write()` — 按需分配新 block，更新 inode size，回写 inode
- `ext2_create(parent, name)` — 分配 inode + 目录项 + 回写
- `ext2_mkdir(parent, name)` — 分配 inode + 初始化 `.`/`..` 目录项
- `ext2_unlink(parent, name)` — 移除目录项 + 释放 block/inode + 回写 bitmap

## 工作目录 & stat (028c)
- per-process `cwd[256]` 字段，初始 `"/"`
- 相对路径解析: 结合 cwd 拼接为绝对路径
- `sys_chdir`: 更新 cwd，验证目标路径存在且为目录
- `sys_getcwd`: 返回 cwd 字符串
- `sys_stat / sys_fstat`: 返回 `struct stat {st_mode, st_size, st_ino, st_type}`

## AHCI 回写
- ext2 修改后的 block/bitmap/inode 通过 `AHCI::write` 回写磁盘

## 系统调用
| 编号 | 功能 |
|------|------|
| `SYS_creat` | 创建文件 |
| `SYS_mkdir` | 创建目录 |
| `SYS_unlink` | 删除文件 |
| `SYS_rmdir` | 删除目录 |
| `SYS_chdir` | 切换工作目录 |
| `SYS_getcwd` | 获取工作目录 |
| `SYS_stat` / `SYS_fstat` | 获取文件元数据 |

## 源码位置
- `kernel/fs/ext2.hpp/cpp` — ext2 驱动
- `kernel/fs/ext2_types.hpp` — ext2 磁盘格式定义
- `kernel/fs/path.hpp/cpp` — 路径解析工具
- `kernel/fs/stat.hpp` — stat 结构体
