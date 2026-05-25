---
title: 028c-fs-cwd-stat-1 · CWD 与 Stat
---

# 028c-1 Hands-on: 路径解析与进程工作目录

## 导语

上一个 tag 我们把 ext2 的写路径打通了——touch 能创建文件、mkdir 能建目录、rm 能删除、echo 能写数据。但如果你在 Shell 里试过 `cd /etc` 然后跑 `cat passwd`，你会发现两件事：第一，cd 根本不存在；第二，就算你用绝对路径 `cat /etc/passwd` 能工作，但一旦你想用相对路径就彻底完蛋——因为内核根本不知道"当前目录"是什么概念。

这个问题的本质在于，我们之前的文件系统操作只认绝对路径。每个 syscall 拿到的路径字符串必须以 `/` 开头，否则 VFS 就不知道你在说什么。但真实的用户空间程序几乎不可能一直用绝对路径——想一想你平时在终端里敲 `vim main.cpp` 的时候，你不会写 `vim /home/charliechen/project/src/main.cpp` 吧？操作系统需要维护一个"进程当前工作目录"（current working directory，CWD），然后在打开文件时把相对路径拼接到 CWD 上，转换成绝对路径再交给 VFS 处理。

这一章我们要做三件事：在进程控制结构里加上 CWD 字段，实现一套路径规范化算法（处理 `.`、`..`、多余的 `/`），然后把这两者组合起来做成一个统一的路径解析工具。完成之后，所有的文件系统 syscall 就都能处理相对路径了。

知识前置：028a（ext2 只读驱动和 VFS 挂载）、028b（ext2 写操作和 Shell 基本命令）。需要理解 Task 结构体、Scheduler 的 current 指针、VFS 挂载表的 resolve 流程。

## 概念精讲

### 进程工作目录 —— "你现在在哪？"

每个 Unix 进程都有一个 CWD，它是一个绝对路径字符串，表示进程当前所处的目录位置。当你执行 `open("foo.txt")` 的时候，内核实际打开的是 `{cwd}/foo.txt`。CWD 不是什么硬件概念，纯粹是内核为每个进程维护的一块软件状态。

在 Linux 里，CWD 存储在 `task_struct->fs->pwd` 中，类型是 `struct path`（包含一个指向 dentry 和 vfsmount 的指针）。Cinux 的做法更简单直接——我们在 `Task` 结构体里直接塞一个 `char cwd[256]` 字段，存的是绝对路径字符串。256 字节对教学内核来说绰绰有余了——Linux 的 PATH_MAX 也是 4096，但我们的路径不会那么长。

进程创建的时候，CWD 初始化为根目录 `"/"`。Cinux 目前还没有 fork/execve，Shell 进程是在 `kernel/arch/x86_64/usermode.cpp` 的 `launch_first_user()` 中通过创建一个 `static Task shell_task{}` 并设置 `.cwd[0] = '/'` 来启动的，所以初始化阶段只要把第一个进程的 CWD 设为 `/` 就行。

### 路径规范化 —— 把乱七八糟的路径收拾干净

用户传入的路径可能千奇百怪：`/a/b/../c`、`/a/./b/./c`、`/a//b///c`、`/a/b/`、`/../../x`。路径规范化（canonicalization）的目标是把所有这些路径都转换成唯一的标准形式——一个从根目录开始的、不包含 `.` 或 `..` 的、不以 `/` 结尾（根目录除外）的绝对路径。

规范化的核心算法是这样的：从左到右扫描路径中的每个"组件"（被 `/` 分隔的字符串），遇到普通组件就压入栈，遇到 `.` 就跳过，遇到 `..` 就弹出栈顶。最后把栈里的组件用 `/` 连起来就是规范路径。这个算法有一个重要的边界条件：`..` 不能弹出根目录——`/../../a` 的规范结果是 `/a`，不是什么"根目录的上一级"。因为在文件系统的语义里，根目录就是最顶层，不存在"往上"的概念。

### 路径解析 —— 把 CWD 和用户路径合体

有了规范化和 CWD 之后，路径解析的逻辑就非常直白了。如果用户传入的路径以 `/` 开头（绝对路径），直接拿去做规范化就行。如果不是以 `/` 开头（相对路径），就把 `cwd + "/" + path` 拼起来再做规范化。比如 cwd 是 `/home/user`，用户传了 `../bin`，拼接后是 `/home/user/../bin`，规范化后是 `/home/bin`。

这里有一个细节值得注意：拼接之前要检查 cwd 本身是不是以 `/` 结尾。如果 cwd 是 `/`（根目录），那直接拼 `/` + `foo` 就是 `//foo`，规范化后是 `/foo`——这没问题。但如果 cwd 是 `/home/user`（不以 `/` 结尾），就要加一个 `/` 分隔符，否则拼出来是 `/home/userfoo`，那就完全不对了。

## 动手实现

### Step 1: 在 Task 结构体中添加 CWD 字段

**目标**: 为进程控制块增加当前工作目录的存储空间。

**设计思路**: 在 `Task` 结构体末尾添加 `char cwd[256]` 字段。选择 256 字节而不是更小的值，是因为要容纳一定深度的目录路径。选择固定大小数组而不是动态分配，是为了避免在内核里引入堆分配的复杂性。字段初始化为 `"/"`。

**实现约束**: 新字段添加在 `fpu_state[512]` 之后。所有创建 Task 的代码路径（包括 `TaskBuilder::build()` 和通过 `fork()` 复制 Task 的路径）都需要确保 cwd 被正确初始化。

**验证**: 编译通过，确认 sizeof(Task) 变化合理：
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -5
```

### Step 2: 实现 path_canonicalize

**目标**: 编写路径规范化函数，能正确处理 `.`、`..`、连续 `/`、尾随 `/` 等情况。

**设计思路**: 使用一个临时缓冲区作为"输出栈"。从左到右扫描输入路径的每个组件，根据组件内容决定操作：`.` 跳过，`..` 回退输出指针到上一个 `/`，普通组件追加到输出缓冲区。最终保证输出一定以 `/` 开头、不以 `/` 结尾（根目录除外）。

**实现约束**: 函数签名为 `void path_canonicalize(char* buf)`，原地修改输入缓冲区。内部使用一个大小为 PATH_MAX（4096）的临时缓冲区，最后 memcpy 回去。需要处理空字符串和 nullptr 的边界情况。

**验证**: 在内核测试里添加单元测试，覆盖以下用例：
- `/a/b/../c` 变成 `/a/c`
- `/a/./b/./c` 变成 `/a/b/c`
- `/a//b///c` 变成 `/a/b/c`
- `/a/b/` 变成 `/a/b`
- `/` 保持 `/`
- `/././.` 变成 `/`
- `/../../a` 变成 `/a`（`..` 不能越过根）

编译并运行测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "path_canonicalize"
```

### Step 3: 实现 path_resolve

**目标**: 实现 cwd 感知的路径解析，将用户传入的（可能为相对）路径转换为规范绝对路径。

**设计思路**: 判断路径首字符是否为 `/`。如果是，直接复制到输出缓冲区后调用 path_canonicalize。如果不是，拼接 `cwd + "/" + path` 后调用 path_canonicalize。拼接时注意 cwd 末尾是否已有 `/`。

**实现约束**: 函数签名为 `bool path_resolve(const char* cwd, const char* path, char* out)`，返回 bool 表示成功/失败。输出缓冲区大小至少 PATH_MAX。需要处理三个参数中任何一个为 nullptr 的情况。

**验证**: 添加测试用例：
- 绝对路径透传：`path_resolve("/", "/usr/bin", out)` 得到 `/usr/bin`
- 相对路径拼接：`path_resolve("/home/user", "docs", out)` 得到 `/home/user/docs`
- `..` 上升：`path_resolve("/home/user", "..", out)` 得到 `/home`
- nullptr 参数返回 false

编译并运行测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "path_resolve"
```

### Step 4: 实现 resolve_user_path 和 validate_user_ptr

**目标**: 为 syscall 层提供统一的路径解析入口，消除各个 syscall 中的重复代码。

**设计思路**: `validate_user_ptr` 做一次 x86_64 规范地址检查（bit 47 扩展到 bit 48-63），防止用户传入内核空间的指针。`resolve_user_path` 在验证指针后，从 `Scheduler::current()` 获取当前进程的 CWD，然后调用 `fs::path_resolve` 完成路径解析。

**实现约束**: `validate_user_ptr` 是 inline 函数，放在头文件中。`resolve_user_path` 的签名为 `bool resolve_user_path(uint64_t path_virt, char* out)`。如果 `Scheduler::current()` 返回 nullptr（理论上不应该发生），则 CWD 回退到 `"/"`。

**验证**: 这个函数被后续所有路径相关 syscall 使用，暂不单独测试。编译通过即可：
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -5
```

### Step 5: Scheduler::set_current 和 CWD 初始化

**目标**: 提供 `set_current()` 方法用于测试中设置当前进程，确保 CWD 在内核启动流程中被正确初始化。

**设计思路**: `Scheduler::set_current(Task* task)` 同时更新 `current_` 静态成员和 `g_per_cpu.current`。Shell 进程在 `launch_first_user()` 中通过创建 `static Task shell_task{}` 并设置 `.cwd[0] = '/'`、`.state = Running`，然后调用 `set_current(&shell_task)` 来注册。测试代码中同样通过 `set_current()` 注册一个 mock Task 来测试 CWD 相关功能。

**验证**: 运行完整内核测试，确认 CWD 初始值测试通过：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "CWD initial"
```

## 小结

到这里，我们已经搭好了路径解析的全部基础设施：Task 里有 CWD 字段，有能处理各种奇葩路径的规范化算法，有 cwd 感知的路径解析器，还有 syscall 层的统一入口。但光有这些还不够——我们还需要能修改 CWD 的 syscall（chdir）、能读取 CWD 的 syscall（getcwd），以及能查询文件元数据的 syscall（stat）。这些就是下一章的内容了。
