---
title: 028c-fs-cwd-stat-3 · CWD 与 Stat
---

# 028c-3 Tutorial: 从内核到 Shell——路径解析统一与命令集成

## 前言

前两章我们实现了路径规范化、进程工作目录、四个新 syscall。但这些能力还锁在内核里——用户空间的 Shell 还没法用它们。而且有一个尴尬的事实：虽然我们实现了 resolve_user_path 这个统一入口，但已有的文件系统 syscall（open、creat、mkdir、unlink、rmdir）还在用老的路径处理方式，根本不支持相对路径。所以这一章要做的就是把所有东西串起来——统一已有 syscall 的路径处理，加上用户空间的 syscall wrapper，最后在 Shell 里实现 cd、pwd、stat 三个命令。

## 消灭重复代码：路径解析的 DRY 重构

在引入 resolve_user_path 之前，每个路径相关 syscall 都自己写一遍指针验证代码。以 sys_creat 为例，重构前是这样的：

```cpp
int64_t sys_creat(uint64_t path_virt, ...) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    if (path_virt == 0) { return -1; }
    uint64_t bit47 = (path_virt >> 47) & 1;
    uint64_t upper = path_virt >> 48;
    if (bit47 == 0 && upper != 0) { return -1; }
    if (bit47 == 1 && upper != 0xFFFF) { return -1; }

    if (path[0] == '\0') { return -1; }

    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(path, &rel_path);
    // ...
}
```

这段 15 行的验证代码在 sys_open、sys_creat、sys_mkdir、sys_unlink、sys_rmdir 中各出现一次——完全一样的逻辑，复制了五次。而且每个文件都有自己的 `constexpr uint32_t PATH_MAX = 4096`。

重构后，所有这些都浓缩成了一行：

```cpp
int64_t sys_creat(uint64_t path_virt, ...) {
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) { return -1; }

    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);
    // ...
}
```

这次重构不只是代码美学——它带来了两个实质性的改进。第一，所有路径相关的 syscall 现在都支持相对路径了——因为 resolve_user_path 内部会从当前进程的 CWD 拼接相对路径。之前 `open("data.txt")` 会失败（因为 vfs_resolve 不认识不以 `/` 开头的路径），现在它会自动解析成 `{cwd}/data.txt`。第二，PATH_MAX 常量统一到了 `cinux::fs::PATH_MAX`，消除了五个不同文件中各自定义的风险。

这种重构在操作系统开发中非常常见。Linux 内核在 `fs/namei.c` 中集中实现了 `do_path_lookup()` / `path_lookupat()` 系列函数，所有需要路径查找的 syscall 都调用它们。SerenityOS 在 `Kernel/FileSystem/VirtualFileSystem.cpp` 中用 `resolve_path()` 做了同样的事情。本质上都是在解决同一个问题——路径解析逻辑应该集中在一个地方，而不是散落在每个 syscall 中。

## 用户空间接口：Syscall Wrapper

用户空间程序通过 syscall 指令调用内核功能。直接写内联汇编容易出错（寄存器约定、clobber 列表、返回值处理），所以我们在 `user/libc/syscall.cpp` 中提供 C 函数 wrapper。

新增的四个 wrapper 非常简单：

```cpp
int64_t sys_chdir(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_chdir), (uint64_t)path);
}

int64_t sys_getcwd(char* buf, size_t size) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_getcwd), (uint64_t)buf, (uint64_t)size);
}

int64_t sys_stat(const char* path, struct sys_stat* st) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_stat), (uint64_t)path, (uint64_t)st);
}

int64_t sys_fstat(int fd, struct sys_stat* st) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_fstat), (uint64_t)fd, (uint64_t)st);
}
```

`_syscall1` 和 `_syscall2` 是共享的内联汇编辅助函数——加载 syscall 号到 RAX，参数到 RDI/RSI，执行 `syscall` 指令，从 RAX 读取返回值。Syscall 号定义在 `kernel/syscall/syscall_nums.hpp` 中，用户空间代码通过 include 共享这些常量。

Syscall 号的选择参考了 Linux 的分配：stat=4, fstat=5, chdir=12, getcwd=79。Cinux 不需要和 Linux 二进制兼容（ABI 完全不同），但使用相同的编号让代码更容易理解。

有一个需要特别注意的点：用户空间的 `struct sys_stat` 和内核的 `struct cinux::fs::stat` 必须有完全相同的内存布局。它们之间通过 syscall 的 memcpy 传递数据，如果字段顺序或大小不一致，数据就会错位。两个结构体的定义如下：

```cpp
// 用户空间 (user/libc/syscall.h)
struct sys_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};
```

在 Linux 中，这个问题通过 glibc 的 `struct stat` 抽象层解决——glibc 定义了自己的 stat 结构体，内部根据内核版本和架构选择正确的布局。Cinux 没有这个间接层，用户空间的定义直接和内核对齐。这是一种简化，但也意味着如果内核的 stat 结构体改了，用户空间必须同步更新。

## Shell 命令：cd、pwd、stat

Shell 命令的实现是对 syscall wrapper 的薄包装。三个命令加起来不到 150 行。

### cd 命令

```cpp
void cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        write_str("cd: missing operand\n");
        return;
    }

    int64_t ret = sys_chdir(argv[1]);
    if (ret < 0) {
        write_str("cd: cannot change to '");
        write_str(argv[1]);
        write_str("'\n");
    }
}
```

cd 是一个 Shell 内建命令（builtin）——它必须由 Shell 进程自身执行，不能 fork 出子进程来做。因为 chdir 改变的是调用进程的 CWD，如果 Shell fork 一个子进程执行 cd，子进程的 CWD 变了但 Shell 自己的 CWD 没变——白忙活。这也是为什么在真实的 Unix Shell 中，cd 永远是 builtin，不是外部程序。

### pwd 命令

```cpp
void cmd_pwd(int, char**) {
    char    buf[256];
    int64_t ret = sys_getcwd(buf, sizeof(buf));
    if (ret < 0) {
        write_str("pwd: failed to get cwd\n");
        return;
    }
    write_str(buf);
    write_str("\n");
}
```

pwd 在技术上也应该是 builtin——因为外部程序无法获取另一个进程的 CWD（Linux 通过 `/proc/self/cwd` 符号链接可以做到，但那需要 procfs）。Cinux 的 pwd 直接调用 sys_getcwd，简单明了。

### stat 命令

stat 命令最复杂，主要是因为需要数字格式化输出：

```cpp
void cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        write_str("stat: missing operand\n");
        return;
    }

    struct sys_stat st;
    int64_t ret = sys_stat(argv[1], &st);
    if (ret < 0) {
        write_str("stat: cannot stat '");
        write_str(argv[1]);
        write_str("'\n");
        return;
    }

    write_str("  File: "); write_str(argv[1]); write_str("\n");
    write_str("  Size: "); write_uint64(st.st_size);
    write_str("\tBlocks: "); write_uint64(st.st_blocks); write_str("\n");
    write_str("  Inode: "); write_uint64(st.st_ino);
    write_str("\tLinks: "); write_uint64(st.st_nlink); write_str("\n");
    write_str("  Mode: "); write_octal(st.st_mode);
    write_str("\tUid: "); write_uint64(st.st_uid);
    write_str("\tGid: "); write_uint64(st.st_gid); write_str("\n");
}
```

`write_uint64` 和 `write_octal` 是自己实现的数字转字符串函数——因为 Cinux 的用户空间还没有 printf。输出格式模仿 Linux 的 `stat` 命令。在 Shell 里执行 `stat /hello.txt` 的效果是：

```
  File: /hello.txt
  Size: 42	Blocks: 2
  Inode: 12	Links: 1
  Mode: 100644	Uid: 0	Gid: 0
```

Mode `100644` 是八进制表示——`010` 表示普通文件（目录则是 `004`），`0644` 是 rw-r--r-- 权限。`write_octal` 输出的是八进制数字（不带前导 `0`），所以 `0100644` 显示为 `100644`。

## 测试覆盖

`kernel/test/test_cwd_stat.cpp` 有 885 行测试代码，24 个测试用例。测试结构清晰，分七组覆盖了从底层算法到 Shell 命令的完整调用链。每组测试都用自己的 `setup_cwd_stat()` / `teardown_cwd_stat()` 管理文件系统资源，确保测试之间不互相干扰。

值得特别提一下的是连续 chdir 测试——先 `cd /dir1`，然后用相对路径 `cd dir2`（不写 `/dir1/dir2`）。这个测试验证了"chdir 改变 CWD -> 下一个 syscall 的路径解析基于新 CWD"这条链路的正确性。如果 resolve_user_path 没有正确读取当前进程的 CWD（比如读到了旧值），这个测试就会失败。

另一个有趣的测试是 stat 和 fstat 的一致性测试——同一个文件，通过路径 stat 和通过文件描述符 fstat，返回的 inode 号和文件大小必须完全一致。这验证了两个 syscall 走的不同代码路径最终都调用了同一个 InodeOps::stat 实现。

## 回顾：028c 的完整架构

```
用户空间                          内核空间
-----------                         ------------
cmd_cd -----> sys_chdir -----> resolve_user_path() --> path_resolve()
cmd_pwd -----> sys_getcwd ----> Scheduler::current()->cwd
cmd_stat ----> sys_stat ------> resolve_user_path() --> VFS --> InodeOps::stat()
              sys_fstat ------> FD table --> InodeOps::stat()
              sys_open  -------> resolve_user_path() --> VFS --> lookup
              sys_creat -------> resolve_user_path() --> VFS --> create
              sys_mkdir -------> resolve_user_path() --> VFS --> mkdir
              sys_unlink ------> resolve_user_path() --> VFS --> unlink
              sys_rmdir -------> resolve_user_path() --> VFS --> unlink
```

所有路径相关的 syscall 都通过 resolve_user_path 统一了入口。这个函数封装了用户指针验证、CWD 获取、路径拼接、规范化这四步操作。下层的 VFS 和文件系统驱动完全不需要知道"相对路径"这个概念——它们收到的永远是规范化的绝对路径。

这和 Linux 的架构是一致的：Linux 在 `fs/namei.c` 的 `path_lookupat()` 中做了同样的事情——把相对路径（通过 `nd->root` 指定的起始目录）解析为 dentry。所有 syscall 通过 `kern_path()` / `user_path_at_empty()` 等接口间接调用它。Cinux 的 resolve_user_path 就是这条调用链的教学版简化。

## 参考

- Linux `fs/namei.c` 路径查找: https://elixir.bootlin.com/linux/latest/source/fs/namei.c
- Linux path resolution (man 7 path_resolution): https://man7.org/linux/man-pages/man7/path_resolution.7.html
- POSIX chdir: https://pubs.opengroup.org/onlinepubs/9699919799/functions/chdir.html
- POSIX stat: https://pubs.opengroup.org/onlinepubs/9699919799/functions/stat.html
- SerenityOS Shell Builtins: https://github.com/SerenityOS/serenity/tree/master/Userland/Shell
