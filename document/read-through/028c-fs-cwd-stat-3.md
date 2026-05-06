# 028c-3 Read-through: Shell 命令集成与验证

## 导语

前两章我们看完了路径解析基础设施和四个新 syscall 的实现。这一章来看最后一块拼图——如何把 resolve_user_path 集成到已有 syscall 中消除代码重复，以及用户空间的 syscall wrapper 和 Shell 命令实现。

## 已有 Syscall 的路径解析统一

以 `sys_open` 为例，来看看重构前后的对比。重构前的代码：

```cpp
int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    // Reject null and non-canonical addresses.
    if (path_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (path_virt >> 47) & 1;
    uint64_t upper = path_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(path, &rel_path);
    // ... 后续逻辑 ...
```

重构后的代码：

```cpp
int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2: Resolve through the VFS mount table
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);
    // ... 后续逻辑 ...
```

变化很简单：删除了 15 行内联的指针验证代码，替换为一行 `resolve_user_path` 调用。同时还获得了两个新能力——对相对路径的支持，以及对 cwd 感知的路径解析。

sys_creat、sys_mkdir、sys_unlink、sys_rmdir 做了完全相同的重构。每个文件都删掉了各自的 `constexpr uint32_t PATH_MAX = 4096` 定义和内联指针验证代码，改用 `cinux::fs::PATH_MAX` 和 `resolve_user_path`。

以 `sys_creat` 为例，完整的重构后代码（关键部分）：

```cpp
int64_t sys_creat(uint64_t path_virt, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2: Resolve through the VFS mount table
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_CREAT] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }

    // Step 3: Split relative path into parent dir and leaf name
    char parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t name_len = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_CREAT] Invalid path: '%s'\n", resolved);
        return -1;
    }
    // ... 后续逻辑不变 ...
```

注意一个变化：原来的 `char parent_buf[PATH_MAX]` 变成了 `char parent_buf[cinux::fs::PATH_MAX]`，因为本地的 PATH_MAX 常量被删除了。

## 用户空间 Syscall Wrapper

`user/libc/syscall.h` 中新增了四个函数声明和 struct sys_stat 定义：

```cpp
int64_t sys_chdir(const char* path);
int64_t sys_getcwd(char* buf, size_t size);

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

int64_t sys_stat(const char* path, struct sys_stat* st);
int64_t sys_fstat(int fd, struct sys_stat* st);
```

`struct sys_stat` 的字段顺序和类型必须和内核的 `cinux::fs::stat` 完全一致——它们通过 syscall 的 memcpy 在用户空间和内核之间传递，内存布局不同的话数据会错位。注意 st_size 是 `int64_t`，其余整数字段都是无符号类型。

`user/libc/syscall.cpp` 中的 wrapper 实现非常机械：

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

每个 wrapper 就是一行——加载 syscall 号到 RAX，参数到 RDI/RSI，执行 syscall 指令，返回 RAX 中的结果。`_syscall1` 和 `_syscall2` 是前面定义的内联汇编辅助函数，分别处理 1 参数和 2 参数的情况。

Syscall 号在 `kernel/syscall/syscall_nums.hpp` 中定义：

```cpp
enum class SyscallNr : uint64_t {
    SYS_read   = 0,
    SYS_write  = 1,
    SYS_open   = 2,
    SYS_close  = 3,
    SYS_stat   = 4,
    SYS_fstat  = 5,
    SYS_chdir  = 12,
    SYS_exit     = 60,
    SYS_yield    = 24,
    SYS_getcwd   = 79,
    SYS_getdents = 78,
    SYS_mkdir    = 83,
    SYS_rmdir    = 84,
    // ...
};
```

编号参考了 Linux 的 syscall 号分配——4 是 stat、5 是 fstat、12 是 chdir、79 是 getcwd。虽然 Cinux 的 syscall 不需要和 Linux 完全兼容，但使用相同的编号有助于理解。

## Shell 命令实现

三个 Shell 命令的实现都非常简洁。cmd_cd（`user/programs/shell/cmd_cd.cpp`）只有 35 行：

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

参数不足时提示错误，调用 sys_chdir，失败时输出错误信息。成功时不输出任何东西——和 bash 的 cd 行为一致。

cmd_pwd（`user/programs/shell/cmd_pwd.cpp`）更短，33 行：

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

256 字节的缓冲区对于 CWD 路径来说绰绰有余。getcwd 成功时返回字节数（含 NUL），失败时返回 -1。

cmd_stat（`user/programs/shell/cmd_stat.cpp`）稍长一些（111 行），但大部分代码是数字格式化辅助函数：

```cpp
void write_uint64(uint64_t val) {
    char buf[21];
    int  pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        uint64_t tmp = val;
        while (tmp > 0) {
            buf[pos++] = '0' + static_cast<char>(tmp % 10);
            tmp /= 10;
        }
    }

    // Reverse
    for (int i = 0; i < pos / 2; ++i) {
        char t           = buf[i];
        buf[i]           = buf[pos - 1 - i];
        buf[pos - 1 - i] = t;
    }

    buf[pos] = '\0';
    write_str(buf);
}

void write_octal(uint32_t val) {
    char buf[12];
    int  pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        uint32_t tmp = val;
        while (tmp > 0) {
            buf[pos++] = '0' + static_cast<char>(tmp & 7);
            tmp >>= 3;
        }
    }

    // Reverse
    for (int i = 0; i < pos / 2; ++i) {
        char t           = buf[i];
        buf[i]           = buf[pos - 1 - i];
        buf[pos - 1 - i] = t;
    }

    buf[pos] = '\0';
    write_str(buf);
}
```

`write_uint64` 把 64 位整数转成十进制字符串，`write_octal` 把 32 位整数转成八进制字符串（用于显示 mode）。两者都是先按逆序生成数字字符，然后翻转。

命令主体的格式化输出：

```cpp
void cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        write_str("stat: missing operand\n");
        return;
    }

    struct sys_stat st;
    int64_t         ret = sys_stat(argv[1], &st);
    if (ret < 0) {
        write_str("stat: cannot stat '");
        write_str(argv[1]);
        write_str("'\n");
        return;
    }

    write_str("  File: ");
    write_str(argv[1]);
    write_str("\n");

    write_str("  Size: ");
    write_uint64(static_cast<uint64_t>(st.st_size));
    write_str("\tBlocks: ");
    write_uint64(st.st_blocks);
    write_str("\n");

    write_str("  Inode: ");
    write_uint64(st.st_ino);
    write_str("\tLinks: ");
    write_uint64(st.st_nlink);
    write_str("\n");

    write_str("  Mode: ");
    write_octal(st.st_mode);
    write_str("\tUid: ");
    write_uint64(st.st_uid);
    write_str("\tGid: ");
    write_uint64(st.st_gid);
    write_str("\n");
}
```

输出格式模仿 Linux 的 `stat` 命令，显示文件名、大小、块数、inode 号、链接数、权限 mode（八进制）、Uid、Gid。在 Shell 里执行 `stat /hello.txt` 的效果类似：

```
  File: /hello.txt
  Size: 42	Blocks: 2
  Inode: 12	Links: 1
  Mode: 100644	Uid: 0	Gid: 0
```

## 测试代码概览

`kernel/test/test_cwd_stat.cpp` 是这个 tag 最长的文件（885 行），包含了 24 个测试用例，覆盖了路径解析、四个 syscall、Shell 命令的完整流程。测试入口函数 `run_cwd_stat_tests()` 首先设置一个 mock Task 作为当前进程：

```cpp
extern "C" void run_cwd_stat_tests() {
    TEST_SECTION("CWD/Stat Tests (028c)");

    cinux::proc::Task test_task;
    for (uint32_t i = 0; i < sizeof(test_task); ++i)
        reinterpret_cast<uint8_t*>(&test_task)[i] = 0;
    test_task.cwd[0] = '/';
    test_task.cwd[1] = '\0';
    cinux::proc::Scheduler::set_current(&test_task);
```

先把整个 Task 清零（包括新增的 cwd 字段），然后设置 cwd 为 "/"，注册到调度器。后续所有测试都通过这个 mock Task 来访问和修改 CWD。

测试分为七个组：CWD 初始值（1 个）、path_canonicalize（7 个）、path_resolve（4 个）、sys_chdir（5 个）、sys_stat（4 个）、sys_fstat（3 个）、Shell 命令（3 个）。其中 path_canonicalize 和 path_resolve 是纯函数测试，不需要文件系统；其余测试需要通过 `setup_cwd_stat()` 初始化 AHCI 和 ext2。

每个涉及 ext2 的测试都调用自己的 `setup_cwd_stat/teardown_cwd_stat`，创建全新的 AHCI 和 ext2 实例。这样做的原因是测试运行在内核态，没有进程隔离——每个测试必须自己管理资源，测试结束要清理干净，避免影响下一个测试。

## 小结

这一章我们看完了 028c tag 的最后一块拼图：五个已有 syscall 通过 resolve_user_path 统一了路径处理逻辑并获得了相对路径支持；用户空间的 syscall wrapper 和 struct sys_stat 定义提供了干净的用户态接口；Shell 的 cd、pwd、stat 三个命令各自只有几十行代码，是对 syscall wrapper 的薄包装。861 行的测试代码全面覆盖了从路径规范化到 Shell 命令的完整调用链。tag 028c 的所有代码变更到这里就全部读完了。
