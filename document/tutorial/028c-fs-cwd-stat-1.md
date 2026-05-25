---
title: 028c-fs-cwd-stat-1 · CWD 与 Stat
---

# 028c-1 Tutorial: 从绝对路径到相对路径——进程工作目录与路径规范化

## 前言

到上一个 tag 为止，Cinux 的 Shell 已经能跑 ls、cat、touch、mkdir、rm、echo 了。但如果你真的坐下来用这个 Shell 干点正事，你会发现一个让人抓狂的问题：所有路径都必须是绝对路径。想看根目录下的文件？`ls /`。想看某个目录下的文件？只能 `ls /dirname`。cd？不存在的。`pwd`？没听说过。

这不仅仅是用户体验的问题——它意味着我们的内核缺少一个 Unix 系统最基本的抽象之一：进程工作目录（Current Working Directory，CWD）。没有 CWD，就没有相对路径；没有相对路径，每一个文件操作都必须从根目录开始写完整路径。这在真实系统中是不可想象的。

这一章我们来补上这个能力。我们会在进程控制结构里存储 CWD，实现一套完整的路径规范化算法，然后把这两者组合成一个统一的路径解析管线。做完这些，Cinux 的文件系统就能真正理解"当前目录"这个概念了。

## Unix 的路径解析模型

在 Unix 系统中，每个进程都有一个 CWD，它是一个绝对路径字符串，表示进程当前所处的目录位置。当你执行 `open("foo.txt")` 的时候，内核实际打开的是 `{cwd}/foo.txt`。CWD 不是硬件概念，纯粹是内核维护的软件状态。

Linux 中 CWD 存储在 `task_struct->fs->pwd` 里，类型是 `struct path`——包含一个指向 dentry 的指针和一个指向 vfsmount 的指针。这是一种引用式的设计，好处是目录被重命名后 CWD 仍然有效（因为引用的是 inode 而不是名字）。SerenityOS 的做法类似，在 `Process` 类中维护一个 `Custody` 指针指向当前的 VFS 节点。Cinux 选择了一个更简单直接的方案——在 Task 结构体里直接存路径字符串。

```cpp
// kernel/proc/process.hpp
struct Task {
    // ... 其他字段 ...
    alignas(16) uint8_t fpu_state[512];

    /** Per-process current working directory (absolute path, NUL-terminated). */
    char cwd[256];
};
```

256 字节对于教学内核来说完全够用。字符串方案比引用方案少了很多复杂性——不需要引用计数，不需要 dentry 缓存，不需要处理挂载命名空间。代价是目录被重命名后 CWD 字符串会失效，但对于 Cinux 来说这是可以接受的 trade-off。

进程创建时 CWD 初始化为根目录。在 `TaskBuilder::build()` 中：

```cpp
// Step 7.5: Initialise cwd to "/"
task->cwd[0] = '/';
task->cwd[1] = '\0';
```

Shell 进程在 `kernel/arch/x86_64/usermode.cpp` 的 `launch_first_user()` 中启动。该函数创建一个 `static cinux::proc::Task shell_task{}`，手动设置 `.cwd[0] = '/'` 和 `.state = Running`，然后通过 `Scheduler::set_current(&shell_task)` 注册为当前进程。这样 Shell 的 CWD 在启动时就被初始化为根目录。

## 路径规范化：把乱七八糟的路径收拾干净

用户传入的路径可以千奇百怪。`/a/b/../c`、`/a/./b/./c`、`/a//b///c`、`/a/b/`、`/../../x`——这些路径在语义上都是合法的，但它们的字面形式各不相同。路径规范化的目标是把所有这些路径转换成唯一的标准形式：从根目录开始、不包含 `.` 或 `..`、不以 `/` 结尾（根目录除外）的绝对路径。

规范化算法的核心思想是"栈式处理"——从左到右扫描路径的每个组件（被 `/` 分隔的字符串），遇到普通组件压栈，遇到 `.` 跳过，遇到 `..` 弹栈。Cinux 的实现用了一个栈上缓冲区来模拟栈操作：

```cpp
void path_canonicalize(char* buf) {
    if (buf == nullptr || buf[0] == '\0') {
        return;
    }

    uint32_t len = static_cast<uint32_t>(strlen(buf));
    char out[PATH_MAX];
    uint32_t out_pos = 0;

    // Always produce a leading '/'
    out[out_pos++] = '/';
```

输出缓冲区总是以 `/` 开头——因为我们处理的都是绝对路径。然后跳过输入中所有前导 `/`，进入主循环逐个处理组件：

```cpp
    while (i < len) {
        uint32_t comp_start = i;
        while (i < len && buf[i] != '/') {
            ++i;
        }
        uint32_t comp_len = i - comp_start;

        // Skip trailing/duplicate slashes
        while (i < len && buf[i] == '/') {
            ++i;
        }

        if (comp_len == 0) continue;

        if (comp_len == 1 && buf[comp_start] == '.') continue;  // "."

        if (comp_len == 2 && buf[comp_start] == '.'
            && buf[comp_start + 1] == '.') {                      // ".."
            if (out_pos > 1) {
                --out_pos;
                while (out_pos > 0 && out[out_pos - 1] != '/') {
                    --out_pos;
                }
                if (out_pos > 1) --out_pos;
            }
            continue;
        }

        // Normal component
        if (out_pos > 0 && out[out_pos - 1] != '/') {
            out[out_pos++] = '/';
        }
        for (uint32_t j = 0; j < comp_len && out_pos < PATH_MAX - 1; ++j) {
            out[out_pos++] = buf[comp_start + j];
        }
    }
```

`..` 的处理是最需要注意的部分。弹栈操作是先退到前一个 `/`，然后根据情况决定是否退掉 `/` 本身。关键的保护条件是 `out_pos > 1`——当输出只有 `/`（根目录）时，`..` 什么也不做。这是 Unix 的语义约定：根目录之上没有父目录，`/../../x` 的结果就是 `/x`。

Linux 内核的 `path_parent_directory()` 在 `fs/namei.c` 中用 `dentry->d_parent` 指针做同样的事情——只不过它是沿着 dentry 树往上走，而不是在字符串上做退格操作。SerenityOS 在 `Kernel/FileSystem/VirtualFileSystem.cpp` 的 `resolve_path` 中用类似的字符串组件栈来实现规范化。

## 路径解析：CWD 感知的完整管线

有了规范化和 CWD 之后，`path_resolve` 负责把两者组合起来：

```cpp
bool path_resolve(const char* cwd, const char* path, char* out) {
    if (cwd == nullptr || path == nullptr || out == nullptr) {
        return false;
    }

    if (path[0] == '/') {
        // Absolute path
        uint32_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) {
            out[i] = path[i]; ++i;
        }
        out[i] = '\0';
        path_canonicalize(out);
        return true;
    }

    // Relative path: cwd + "/" + path
    uint32_t pos = 0;
    while (cwd[pos] != '\0' && pos < PATH_MAX - 2) {
        out[pos] = cwd[pos]; ++pos;
    }
    if (pos > 0 && out[pos - 1] != '/' && pos < PATH_MAX - 2) {
        out[pos++] = '/';
    }
    uint32_t j = 0;
    while (path[j] != '\0' && pos < PATH_MAX - 1) {
        out[pos++] = path[j++];
    }
    out[pos] = '\0';

    path_canonicalize(out);
    return true;
}
```

绝对路径直接规范化，相对路径先拼接再规范化。拼接时注意 cwd 末尾是否已有 `/`——如果 cwd 是 `/`（根目录），不加分隔符；如果是 `/home/user`，要加 `/`。

最后一层封装是 `resolve_user_path`——它在 syscall 层提供统一的入口，加上用户指针验证和 CWD 获取：

```cpp
bool resolve_user_path(uint64_t path_virt, char* out) {
    if (!validate_user_ptr(path_virt)) return false;
    auto* path = reinterpret_cast<const char*>(path_virt);
    if (path[0] == '\0') return false;

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char* cwd = (current != nullptr) ? current->cwd : "/";
    return cinux::fs::path_resolve(cwd, path, out);
}
```

`validate_user_ptr` 做的是 x86_64 规范地址检查——bit 47 必须符号扩展到 bit 48-63。这防止了用户空间程序通过传入内核地址来读写内核内存。

## 和 Linux 对比

Linux 的路径解析在 `fs/namei.c` 中，大约 3000 行代码。它处理的东西比 Cinux 多得多——符号链接跟随、挂载点穿越、命名空间隔离、ACL 检查、OV-SETID 能力限制等。Cinux 的 path.cpp 只有 133 行，但涵盖了路径解析最核心的 80% 功能。剩下 20% 的复杂度（符号链接和挂载穿越）是 Linux 那几千行代码的主要来源。

Linux 在 `path_lookupat()` 中使用 `struct nameidata` 来维护路径查找的中间状态（当前 dentry、挂载点、符号链接深度计数等），而不是像 Cinux 这样用纯字符串操作。dentry 级别的操作比字符串操作效率更高（不需要反复解析路径），但也更复杂。Cinux 的字符串方案在教学上下文中是更好的选择——概念清晰，代码可读，容易测试。

## 参考

- Linux `fs/namei.c` 路径解析: https://elixir.bootlin.com/linux/latest/source/fs/namei.c
- Linux `getcwd(2)`: https://man7.org/linux/man-pages/man2/getcwd.2.html
- POSIX.1-2017 path resolution: https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap04.html#tag_04_13
- SerenityOS VirtualFileSystem: https://github.com/SerenityOS/serenity/blob/master/Kernel/FileSystem/VirtualFileSystem.cpp
