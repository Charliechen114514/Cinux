# 028c-3 Hands-on: Shell 命令集成与验证

## 导语

前两章我们完成了所有底层基础设施：路径规范化算法、cwd 感知的路径解析器、chdir/getcwd/stat/fstat 四个新 syscall。但如果你现在就去 Shell 里试 cd 和 pwd，还是用不了——因为还有两件事没做。第一，现有的文件系统 syscall（open、creat、mkdir、unlink、rmdir）还在用老的方式处理路径，它们不认识相对路径。第二，用户空间的 Shell 里还没有 cd、pwd、stat 这几个命令。

这一章我们要做的是把所有东西串起来：把已有 syscall 的路径处理逻辑统一换成 resolve_user_path，加上用户空间的 syscall wrapper 和 Shell 命令实现，最后跑一遍完整的集成测试验证所有功能。这章的代码改动看起来很散（涉及十几个文件），但本质上就是在做同一件事——让整个系统从"只认绝对路径"变成"支持相对路径"。

知识前置：028c-1 和 028c-2（路径解析基础设施和四个新 syscall）。需要理解 syscall 的注册机制、用户空间 wrapper 的编写方式、Shell 命令的分发框架。

## 概念精讲

### 路径解析统一 —— 消灭六处重复代码

在实现 resolve_user_path 之前，每个路径相关的 syscall 都自己写一遍"验证用户指针 + 检查空字符串"的代码。具体来说，sys_open、sys_creat、sys_mkdir、sys_unlink、sys_rmdir 这五个 syscall 各自有一段几乎一模一样的指针验证逻辑——检查地址不是 0、检查 bit 47 扩展是否正确、检查字符串不为空。这段代码大概 15 行，复制了 5 次。

引入 resolve_user_path 之后，这些 syscall 不再需要自己处理指针验证和路径解析。它们只需要调用 `resolve_user_path(path_virt, resolved)` 一行代码就能完成所有前置工作。这意味着那些 syscall 可以删掉各自的 PATH_MAX 常量定义（之前每个文件自己定义一遍 `constexpr uint32_t PATH_MAX = 4096`），删掉 validate_user_ptr 的内联代码，改用 `cinux::fs::PATH_MAX` 统一常量。

这种重构不是什么花哨的设计模式，就是最基本的 DRY（Don't Repeat Yourself）原则。但如果你回头看看重构前的代码——五个文件各有一份几乎相同的验证代码，改一个地方要同步改五个文件——你就会理解为什么 Linux 内核要在 namei.c 里集中实现路径查找逻辑，而不是让每个 syscall 自己搞一套。

### 用户空间 Syscall Wrapper

用户空间程序要通过 syscall 指令调用内核功能，但直接写内联汇编既容易出错又不好维护。所以我们在 `user/libc/syscall.cpp` 里提供 C 函数 wrapper——每个 wrapper 加载对应的 syscall 号到 RAX，设置参数寄存器（RDI、RSI、RDX 等），执行 syscall 指令，然后从 RAX 读返回值。

新增的 wrapper 包括 `sys_chdir`（1 个参数）、`sys_getcwd`（2 个参数）、`sys_stat`（2 个参数）、`sys_fstat`（2 个参数）。它们的实现方式非常机械——就是调用 `_syscall1` 或 `_syscall2` 辅助函数，传入对应的 SyscallNr 枚举值和参数。用户空间的 `struct sys_stat` 定义必须和内核的 `struct cinux::fs::stat` 内存布局完全一致，否则拷贝过来的数据会对不上。

### Shell 命令实现

Shell 命令的实现本质上是"syscall wrapper 的薄包装"。cmd_cd 调用 sys_chdir，cmd_pwd 调用 sys_getcwd 然后把结果写到 stdout，cmd_stat 调用 sys_stat 然后把结构体字段格式化输出。三个命令加起来代码量不到 150 行，逻辑都很直白。

cmd_stat 的输出格式模仿了 Linux 的 stat 命令，显示 File name、Size、Blocks、Inode、Links、Mode（八进制）、Uid、Gid 这些信息。需要自己实现 write_uint64 和 write_octal 两个辅助函数来做数字到字符串的转换，因为 Cinux 的用户空间还没有 printf。

## 动手实现

### Step 1: 重构 sys_open 使用 resolve_user_path

**目标**: 将 sys_open 的路径处理逻辑替换为统一的 resolve_user_path 调用。

**设计思路**: 删除 sys_open 中内联的指针验证代码和空字符串检查，替换为 `resolve_user_path(path_virt, resolved)`。后续所有使用 path 的地方改为使用 resolved。引入 `kernel/fs/path.hpp` 和 `kernel/syscall/path_util.hpp` 头文件。删除本地的 `PATH_MAX` 常量，改用 `cinux::fs::PATH_MAX`。

**验证**: 编译通过。已有的 open 相关测试应该不受影响：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "open"
```

### Step 2: 重构 sys_creat、sys_mkdir、sys_unlink、sys_rmdir

**目标**: 对其余四个路径相关 syscall 做同样的重构。

**设计思路**: 和 sys_open 完全相同的模式——删除内联验证代码，替换为 resolve_user_path，引入 path.hpp 和 path_util.hpp，删除本地 PATH_MAX。每个文件都减掉大约 15 行重复代码。

**验证**: 编译通过，已有的 syscall 测试全部通过：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep -E "creat|mkdir|unlink|rmdir"
```

### Step 3: 添加用户空间 Syscall Wrapper

**目标**: 在 user/libc/syscall.h 和 syscall.cpp 中添加新 syscall 的用户空间接口。

**设计思路**: syscall.h 中声明 sys_chdir、sys_getcwd、sys_stat、sys_fstat 函数和 struct sys_stat 结构体。syscall.cpp 中用 _syscall1 和 _syscall2 实现这些 wrapper，传入对应的 SyscallNr 枚举值。sys_stat 结构体的字段和类型必须与内核的 cinux::fs::stat 完全一致。

**验证**: 编译 Shell 程序：
```bash
cd build && cmake .. && make shell 2>&1 | tail -5
```

### Step 4: 实现 cmd_cd 命令

**目标**: 实现 Shell 内置的 cd 命令。

**设计思路**: 解析参数拿到目标路径（argv[1]），调用 sys_chdir。如果参数不足，输出错误提示。如果 chdir 失败，输出错误信息。成功时无输出（和 bash 的行为一致）。

**验证**: 启动内核后在 Shell 里执行 `cd /` 然后用 getcwd 验证工作目录。或者运行集成测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "shell cd"
```

### Step 5: 实现 cmd_pwd 命令

**目标**: 实现 Shell 内置的 pwd 命令。

**设计思路**: 调用 sys_getcwd 获取当前工作目录字符串，写入 stdout，后面跟一个换行。如果 getcwd 失败，输出错误信息。缓冲区大小 256 字节。

**验证**: 运行集成测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "shell pwd"
```

### Step 6: 实现 cmd_stat 命令

**目标**: 实现 Shell 内置的 stat 命令，显示文件元数据。

**设计思路**: 调用 sys_stat 获取 struct sys_stat，然后逐字段格式化输出。需要实现 write_uint64（十进制）和 write_octal（八进制）辅助函数来输出数字。输出格式包含 File name、Size、Blocks、Inode、Links、Mode、Uid、Gid。

**验证**: 运行集成测试：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep "shell stat"
```

### Step 7: 完整集成测试验证

**目标**: 运行所有 028c 测试，确认全部通过。

**验证**: 运行完整的 CWD/stat 测试套件（覆盖 path canonicalize、path resolve、chdir、getcwd、stat、fstat、shell 命令），确认 24 个测试全部通过：
```bash
cd build && cmake .. && make big_kernel_test 2>&1 | tail -10
./big_kernel_test 2>&1 | grep -A5 "CWD/Stat Tests"
```

确认最终的测试输出中 028c 相关的所有测试都是 PASS 状态。可以关注串口输出的以下关键日志：
- `[CWD_STAT] chdir to ... OK` — chdir 到新目录成功
- `[CWD_STAT] consecutive chdir -> ... OK` — 连续 chdir 使用相对路径成功
- `[CWD_STAT] stat / -> ino=2` — stat 根目录返回正确的 inode 号
- `[CWD_STAT] fstat fd=... -> ino=... size=...` — fstat 工作正常
- `[CWD_STAT] stat vs fstat: ino=.../... size=.../... OK` — stat 和 fstat 结果一致

## 小结

到这里，tag 028c 的全部工作完成了。现在 Shell 里可以 `cd /etc`，可以 `pwd`，可以 `stat /hello.txt`。所有文件系统 syscall 都支持了相对路径——`open("data.txt")` 不再报错，而是会基于当前工作目录去查找文件。

回顾一下我们做了什么：进程结构体多了 CWD 字段，路径规范化算法处理了 `.` `..` 等特殊组件，路径解析器把 CWD 和用户路径合体，resolve_user_path 统一了所有 syscall 的入口逻辑，四个新 syscall 让进程能切换和查询工作目录、查询文件元数据，Shell 里多了三个命令。整个链条从 Task 结构体一路通到用户空间命令行，每一层都各司其职。

下一步自然就是给内核的共享数据结构加锁了——不过那是 028d 的事情。
