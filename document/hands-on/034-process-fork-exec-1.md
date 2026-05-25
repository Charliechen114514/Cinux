---
title: 034-process-fork-exec-1 · Fork 与 Exec
---

# ELF 格式解析与 PID 分配器

## 导语

到这里我们已经有了一个可以跑用户态程序、有 syscall 机制、有文件系统和 GUI 桌面的内核。但所有进程都在同一个 shell 里打转——我们还没有创建新进程的能力。接下来我们要做的，就是给内核加上 fork、execve 这些 UNIX 进程原语，让它真正拥有多进程的能力。

本篇聚焦在最底层的两个基础设施上：ELF 可执行文件的格式解析，以及进程 ID 的分配管理。这两个东西是后续 fork 和 execve 的地基——execve 需要从文件系统读取 ELF 并加载到内存，而 fork 和 getpid 需要一个可靠的 PID 来源。

完成本篇后，我们会在内核中拥有一个经过严格验证的 ELF 解析模块和一个线程安全的 PID 分配器，为下一篇的 fork 做好准备。

## 概念精讲

### ELF64 格式

ELF（Executable and Linkable Format）是 Linux 世界最通用的可执行文件格式。一个 ELF 文件的开头是 64 字节的文件头（Elf64_Ehdr），里面包含了整个文件的元数据：魔数（0x7F 'E' 'L' 'F'）、目标架构（x86-64 对应 machine=62）、文件类型（executable=2）、入口点地址、以及程序头表的位置和大小。

程序头表紧跟在文件头之后，每个条目 56 字节，描述一个需要加载到内存的段。其中最关键的是 PT_LOAD 类型的段——它告诉内核"把这个文件偏移处的数据，复制到这个虚拟地址去"。一个典型的静态链接可执行文件有两个 PT_LOAD 段：一个是只读的代码段（.text + .rodata），另一个是可读写的数据段（.data + .bss）。

这里需要注意 p_filesz 和 p_memsz 的区别：p_filesz 是文件中实际有的数据量，p_memsz 是内存中需要分配的空间。差值部分（通常是 .bss 段）需要清零。我们的策略是先给整页清零再填入文件数据，这样 .bss 自然就是零。

### PID 管理

PID（Process ID）是操作系统给每个进程分配的唯一数字标识。在 UNIX 语义中，PID 从 1 开始递增（init 进程永远是 1），进程退出后 PID 可以回收重用。PID 0 通常保留给 idle 进程，不分配给任何用户进程。

我们需要一个 PID 分配器来管理这个有限的数字空间。Cinux 选择的上限是 256（PID_MAX），这对教学内核来说绰绰有余。分配策略采用 hint-based 线性扫描：记住上次分配的位置，下次从那里继续往后找，找到第一个空闲的 PID 就分配。释放 PID 时如果发现它比 hint 小，就把 hint 拉回来，这样下次分配能更快地复用低号 PID。

## 动手实现

### Step 1: ELF 类型定义

**目标**: 定义 ELF64 文件头和程序头的内存布局，以及验证结果枚举。

**设计思路**: 我们需要一个独立的命名空间来放所有 ELF 相关的类型，避免和内核其他部分的名字冲突。文件头和程序头必须和磁盘上的二进制布局完全一致——也就是说结构体必须是 packed 的，大小分别是精确的 64 字节和 56 字节。我们用 static_assert 在编译期保证这一点。

**实现约束**: 定义一个命名空间 `cinux::proc::elf`，里面放所有 ELF 常量（magic number 0x464C457F、class 64-bit、data encoding little-endian、machine x86-64=62、type executable=2、PT_LOAD=1、PF_R/PF_W/PF_X 标志位）。然后定义 Elf64_Ehdr 和 Elf64_Phdr 两个 packed struct，包含对应标准字段。最后定义一个 scoped enum ElfValidateResult，列出所有可能的验证失败原因（magic 错误、不是 64-bit、不是小端、不是 x86-64、不是可执行文件、program header 偏移越界、program header entry size 错误、没有 program header）。

**踩坑预警**: e_ident 数组是 16 字节，前 4 字节是魔数，第 5 字节是 class（1=32-bit, 2=64-bit），第 6 字节是 data encoding（1=little-endian, 2=big-endian）。验证魔数时要组装成 uint32_t 比较，注意字节顺序——我们的魔数常量是 0x464C457F，这正好是 'F','L','E',0x7F 的小端表示。不要用 memcmp，避免引入字符串函数依赖。

**验证**: 编译通过即可。如果你的 static_assert 触发了，说明结构体布局和标准不一致。运行 `cmake --build build` 检查编译是否通过。

### Step 2: ELF 头验证函数

**目标**: 实现一个函数，接收指向 ELF 数据的指针和总大小，返回验证结果。

**设计思路**: 按照严格的顺序逐一检查 ELF header 的各个字段。一旦某个检查失败就立即返回对应的错误码，不继续往下检查。这样调用者能根据错误码知道具体是什么问题——是根本不是 ELF 文件（BadMagic），还是虽然是 ELF 但不是 x86-64 的（BadMachine），还是类型不对（BadType）。

**实现约束**: 函数签名接收一个指向 Elf64_Ehdr 的指针和一个表示总文件大小的 uint64_t。首先检查总大小是否至少能容纳一个 ELF header（64 字节），然后依次检查魔数、class=2、data=1、machine=62、type=2、phoff 不超过文件大小、phentsize 等于 56、phnum 不为 0。

**踩坑预警**: phoff 是 program header table 在文件中的偏移量，不是虚拟地址。如果 phoff 大于文件大小，说明 header 声明的 program header table 根本不在文件里，这是一个损坏的 ELF 文件。phentsize 必须精确等于 sizeof(Elf64_Phdr) = 56。

**验证**: 写一个 host 端单元测试，构造一个合法的 ELF header buffer 验证通过，然后分别改掉 magic / class / machine / type 验证返回对应错误码。测试文件放在 `kernel/test/test_fork_exec.cpp` 或 `test/unit/test_fork_exec.cpp` 中。运行 `build/test/unit_test` 验证。

### Step 3: PID 分配器

**目标**: 实现一个类来管理 PID 的分配和释放。

**设计思路**: 用一个 bool 数组记录每个 PID 是否在使用中。分配时从上次分配位置的下一个开始扫描，找到第一个空闲的 PID 就标记为已使用并返回。如果扫描了一圈全是已使用的，说明 PID 耗尽，返回 0。释放时标记为未使用，如果这个 PID 比 hint 小就拉回 hint。

**实现约束**: 类名叫 PidAllocator，放在 `cinux::proc` 命名空间。常量 PID_MAX=256、PID_NONE=0。需要 alloc()、free(pid)、is_allocated(pid)、count() 四个方法。私有成员是一个 bool 数组（大小 PID_MAX+1，因为索引 0 到 256）和一个 int next_hint_。构造函数把所有标记清 false，hint 设为 1。分配时从 next_hint_ 开始线性扫描 PID_MAX 个位置，用取模方式回绕到 1（跳过 0）。还需要声明一个全局实例 `extern PidAllocator g_pid_alloc`。

**踩坑预警**: 分配函数里 candidate 的回绕计算要小心。`candidate = next_hint_ + i`，如果超过 PID_MAX 就减去 PID_MAX。但如果减完后变成了 0，要强制变成 1（因为 PID 0 不分配）。另外 free() 要检查边界（pid <= 0 || pid > PID_MAX 是无效的）和幂等性（释放一个已经空闲的 PID 应该是 no-op）。

**验证**: Host 端测试分配递增、释放后复用、分配耗尽返回 0、释放无效 PID 是 no-op、count() 计数正确。运行 `build/test/unit_test` 验证。

### Step 4: 系统调用注册

**目标**: 给 getpid 和 getppid 注册系统调用。

**设计思路**: 这两个是最简单的系统调用——直接从当前任务的 TCB 中读取 pid 和 ppid 字段返回就行了。sys_getpid 返回 `Scheduler::current()->pid`，sys_getppid 返回 `Scheduler::current()->ppid`。每个系统调用一个独立的 .hpp/.cpp 文件，在 syscall 分发表中注册。

**实现约束**: 函数签名遵循统一的 6 参数格式 `int64_t sys_xxx(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)`，getpid 和 getppid 不需要参数，全部忽略。注册到对应的 SyscallNr 枚举值（SYS_getpid=39, SYS_getppid=110）。在 `kernel/arch/x86_64/syscall.cpp` 中添加 include 和 syscall_register 调用。

**验证**: 在 QEMU 内核测试中创建一个任务，设置它的 pid 和 ppid，然后通过 syscall 调用 getpid/getppid 验证返回值。运行 `make run-kernel-test`。

## 构建与运行

构建命令和之前一样：

```bash
cmake --build build
make run
```

如果只想跑 host 端测试：`build/test/unit_test`。QEMU 内核测试：`make run-kernel-test`。

## 调试技巧

1. **ELF 验证失败**: 如果 execve 报 ELF validation failed，先打印 e_ident 的前 16 字节看是不是真的是 ELF 文件。常见错误是把目录文件当作可执行文件传给了 execve。

2. **PID 耗尽**: 如果 fork 返回 -1 并且串口打印 "[PROC] fork: PID allocator exhausted"，说明有进程在退出后没有调用 free() 回收 PID。检查 waitpid 和 exit 路径是否正确释放了 PID。

3. **getpid 返回 0**: 说明任务的 pid 字段没有被初始化。检查 TaskBuilder::build() 是否在创建任务时分配了 PID 并赋值。

## 本章小结

| 组件 | 关键设计 | 对应源文件 |
|------|----------|-----------|
| Elf64_Ehdr | 64 字节 packed struct, static_assert 校验 | kernel/proc/elf_types.hpp |
| Elf64_Phdr | 56 字节 packed struct, PT_LOAD 段描述 | kernel/proc/elf_types.hpp |
| validate_elf_header | 顺序检查 8 个字段 | kernel/proc/elf_types.cpp |
| PidAllocator | hint-based 线性扫描, PID_MAX=256 | kernel/proc/pid.hpp, kernel/proc/pid.cpp |
| sys_getpid / sys_getppid | 直接读 TCB 字段 | kernel/syscall/sys_getpid.cpp, kernel/syscall/sys_getppid.cpp |
