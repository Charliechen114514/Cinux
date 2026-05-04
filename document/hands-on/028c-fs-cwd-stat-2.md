# 028c-2 Hands-on: sys_chdir / sys_getcwd 与 stat 实现

## 导语

上一章我们把路径解析的基础设施全部搭好了——Task 里有 CWD，path_canonicalize 能处理 `.` `..`，path_resolve 能做 cwd 感知的路径拼接，resolve_user_path 给 syscall 层提供了统一入口。但这些目前都还是"基础设施"，用户空间的程序没法调用它们。我们需要通过 syscall 把这些能力暴露出去。

这一章要做的事情其实很直观：实现 chdir 让进程能切换工作目录，实现 getcwd 让进程能读取自己的工作目录，实现 stat/fstat 让进程能查询文件的元数据信息。这三个 syscall 看起来各自独立，但它们共享了上一章搭建的路径解析基础设施——chdir 和 stat 都需要先通过 resolve_user_path 把用户传入的路径解析成绝对路径，然后才能做后续操作。而 stat 的实现还涉及到 InodeOps 层的扩展——我们需要在 ext2 驱动里加上从磁盘 inode 读取元数据的代码。

知识前置：028c-1（路径解析和进程工作目录）、028a/028b（ext2 驱动和 VFS）。需要理解 syscall 的注册和分发机制、InodeOps 虚函数表的设计、ext2 磁盘 inode 的字段布局。

## 概念精讲

### chdir —— "我要搬到这个目录"

chdir 的工作流程非常清晰：拿到用户传入的路径，解析成绝对路径，通过 VFS 查找对应的 inode，检查这个 inode 是不是一个目录（不能 chdir 到一个普通文件上），最后把当前进程的 CWD 更新为解析后的绝对路径。整个过程中任何一步失败都要返回错误，并且保证 CWD 不被修改。

有一个设计上的选择值得说一说：chdir 更新 CWD 的时候，是存 inode 编号还是存路径字符串？Linux 的做法是存 dentry/vfsmount 指针（相当于存 inode 引用），好处是不怕目录被重命名——因为你引用的是 inode 而不是名字。Cinux 的做法是存路径字符串，这更简单，但也有一个明显的缺点：如果有人把你正在用的目录重命名了，你的 CWD 字符串就指向一个不存在的路径了。不过对于教学内核来说，这个 trade-off 是完全可以接受的——字符串方案代码量少，概念清晰，不需要额外的引用计数机制。

### getcwd —— "我现在在哪？"

getcwd 的实现简直简单到有点不好意思——就是从当前进程的 CWD 字段里把字符串拷贝到用户提供的缓冲区中。但就是这么简单的操作，也有需要注意的地方：用户提供的缓冲区可能不够大。如果 CWD 是 `/a/very/deeply/nested/directory/path`（39 字节加 NUL 共 40 字节），但用户只给了 30 字节的缓冲区，那就不能硬写——必须返回错误。getcwd 成功时返回的是写入的字节数（包括 NUL），不是 0——这一点跟大多数 syscall 不同。

### stat / fstat —— "告诉我这个文件的详细信息"

stat 系统调用是 Unix 里查询文件元数据的标准接口。给定一个路径（stat）或文件描述符（fstat），它返回一个 struct stat 结构体，里面包含文件的大小、inode 号、权限、所有者、时间戳等信息。这个结构体的字段布局在不同 Unix 系统上不完全一样，但核心字段基本一致。Cinux 的 struct stat 遵循 Linux x86_64 的惯例。

stat 的实现分为三层。syscall 层负责路径解析、VFS 查找、结果拷贝——这些是通用的。InodeOps 层定义了一个虚函数 `stat()`，让不同的文件系统驱动各自实现。ext2 层从缓存的磁盘 inode 中读取字段填充 struct stat。这种分层设计意味着将来如果加了新的文件系统（比如 ramfs 或者 FAT32），只需要实现自己的 `InodeOps::stat()` 就行，syscall 层的代码完全不用动。

ext2 磁盘 inode 中包含的信息比 struct stat 需要的多得多——我们只需要 mode（类型+权限）、uid、gid、size、nlink、blocks、三个时间戳这些字段。这些字段直接从 Ext2CachedInode 中的 disk_inode 结构体拷贝到 struct stat 就行，不需要额外的磁盘 I/O，因为 ext2 在 lookup 的时候已经把整个 inode 都读进内存了。

## 动手实现

### Step 1: 定义 struct stat

**目标**: 定义文件状态结构体，遵循 Linux x86_64 的字段惯例。

**设计思路**: 结构体放在 `fs/stat.hpp` 中，使用 `cinux::fs` 命名空间。字段包括 st_dev、st_ino、st_mode、st_nlink、st_uid、st_gid、st_rdev、st_size、st_blksize、st_blocks、st_atime、st_mtime、st_ctime。类型尽量和 Linux 一致（uint64_t、uint32_t、int64_t）。用户空间的 `sys_stat` 结构体定义要和内核版本保持内存布局一致。

**验证**: 编译通过，确认内核和用户空间的 struct 布局匹配：
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -5
```

### Step 2: InodeOps 扩展 stat 虚函数

**目标**: 在 InodeOps 基类中添加 `virtual int64_t stat(const Inode*, struct stat*)` 方法。

**设计思路**: 基类中的默认实现返回 -1（不支持）。Ext2FileOps 和 Ext2DirOps 各自重写此方法，从 Ext2CachedInode 的 disk_inode 中读取字段填充 struct stat。Inode 结构体本身也增加 mode/uid/gid/nlink/atime/ctime/mtime/blocks 字段，在 populate_vfs_inode 时从磁盘 inode 填充。

**实现约束**: stat 方法接收的是 const Inode 指针，通过 inode->fs_private 转型为 Ext2CachedInode 指针来访问磁盘数据。需要检查 nullptr。Ext2FileOps::stat 和 Ext2DirOps::stat 的实现完全相同（都从 disk_inode 读取），可以考虑将来重构为共享代码。

**验证**: 编译通过：
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -5
```

### Step 3: 实现 sys_chdir

**目标**: 实现切换当前工作目录的系统调用。

**设计思路**: 调用 resolve_user_path 解析路径（处理相对路径），通过 vfs_resolve 找到文件系统实例，调用 fs->lookup 查找 inode，验证 inode->type 是 Directory，最后把解析后的绝对路径拷贝到 current->cwd 中。

**实现约束**: 路径长度不能超过 sizeof(cwd) - 1（255 字节）。任何一步失败（路径不存在、不是目录、没有当前进程）都返回 -1，且不修改 cwd。返回 0 表示成功。

**验证**: 编写集成测试——创建目录、chdir 进去、验证 cwd 被更新：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "chdir"
```

测试用例应覆盖：chdir 到存在的目录（成功）、chdir 到不存在的路径（失败）、chdir 到普通文件（失败）、连续 chdir 使用相对路径。

### Step 4: 实现 sys_getcwd

**目标**: 实现获取当前工作目录的系统调用。

**设计思路**: 获取当前 Task 指针，计算 cwd 字符串长度（含 NUL），检查用户缓冲区大小是否足够，然后用 memcpy 拷贝过去。成功返回字节数（含 NUL），失败返回 -1。

**实现约束**: 需要做用户指针的规范地址检查。size 参数为 0 时返回错误。

**验证**: 测试——先 chdir 到已知目录，再 getcwd，验证返回的字符串和预期一致：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "getcwd"
```

### Step 5: 实现 sys_stat 和 sys_fstat

**目标**: 实现基于路径和基于文件描述符的文件状态查询系统调用。

**设计思路**: sys_stat 的流程是 resolve_user_path -> vfs_resolve -> fs->lookup -> inode->ops->stat -> memcpy 到用户缓冲区。sys_fstat 的流程是 fd_table.get(fd) -> file->inode -> inode->ops->stat -> memcpy。两者共享 "拿到 inode 后调用 stat" 的后半段逻辑。

**实现约束**: 需要验证用户传入的 struct stat 指针是规范地址。调用 InodeOps::stat 之前检查 ops 不为 nullptr。内核侧先填充一个临时的 stat 结构体，然后再整体拷贝到用户空间。

**验证**: 编写测试覆盖——stat 根目录（inode 号应为 2）、stat 普通文件（验证 size 和 inode 号）、stat 不存在的文件（返回 -1）、stat 相对路径（先 chdir 再 stat）、fstat 已打开的文件、fstat 和 stat 结果一致性、fstat 无效 fd：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep -E "stat|fstat"
```

### Step 6: 注册 syscall 号

**目标**: 在 syscall 分发表中注册新的系统调用号。

**设计思路**: 在 syscall_nums.hpp 中添加枚举值 SYS_stat=4、SYS_fstat=5、SYS_chdir=12、SYS_getcwd=79。这些编号参考了 Linux 的 syscall 号分配（Linux 的 chdir 是 12、getcwd 是 79、stat 是 4、fstat 是 5）。在 register_builtin_handlers 中注册对应的 handler 函数。

**验证**: 编译并运行完整测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "028c"
```

## 小结

到这里，chdir、getcwd、stat、fstat 四个 syscall 全部实现完毕。进程现在可以切换和查询自己的工作目录了，也可以查询文件的大小、inode 号、权限等元数据。但还有一个重要的事情没做——让已有的 syscall（open、creat、mkdir、unlink、rmdir）也支持相对路径，并在 Shell 里加上 cd、pwd、stat 命令。这些集成工作就是下一章的内容了。
