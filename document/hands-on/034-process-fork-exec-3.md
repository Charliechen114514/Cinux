---
title: 034-process-fork-exec-3 · Fork 与 Exec
---

# execve 与进程生命周期

## 导语

上一篇我们实现了 fork，子进程可以和父进程跑同一份代码了。但真正的多进程世界里，fork 之后通常紧跟 execve——用一个全新的程序替换当前进程的内存映像。这一篇我们把 execve、waitpid、以及进程的完整生命周期（创建 -> 运行 -> 退出 -> 回收）全部串起来。

完成本篇后，内核就拥有了完整的 UNIX 进程三件套：fork 创建子进程、execve 加载新程序、waitpid 回收退出的子进程。这是我们多终端桌面的最后一块基石。

## 概念精讲

### execve 的 ELF 加载流程

execve 做的事情可以用一句话概括：把当前进程的地址空间清空，从文件系统读取一个新的 ELF 可执行文件，把它的 PT_LOAD 段映射到地址空间，然后把入口点地址写入 task->ctx.rip。调用者（通常是 create_shell_terminal 里的子进程代码）在 execve 返回后负责跳转到用户态，CPU 就从新的入口点开始执行。

ELF 加载的核心逻辑是遍历每个 PT_LOAD 段，对段覆盖的每一页虚拟内存：分配一个物理页、清零、从 ELF 文件中复制数据（如果这一页有对应的文件数据的话）、然后 map 到地址空间。需要特别注意的是 p_filesz 和 p_memsz 的区别——p_filesz 是文件中实际有的数据量，p_memsz 是内存中需要分配的空间。差值部分（通常是 .bss 段）需要清零。我们的策略是先给整页清零再填入文件数据，.bss 自然就是零。

### 进程状态机

Cinux 的进程有五个状态：Ready（已创建，等待调度）、Running（正在 CPU 上跑）、Blocked（等待 I/O 或锁）、Zombie（已退出，等父进程 waitpid）、Dead（已被回收，TCB 可以释放）。创建时是 Ready，调度器选中后变 Running，等 I/O 时变 Blocked，调用 exit 后变 Zombie，父进程 waitpid 后变 Dead。

### waitpid 与 Zombie 回收

当一个进程调用 exit() 退出后，它变成 Zombie 状态——TCB 还在内存里（因为父进程可能要读取 exit_status），但不再参与调度。父进程调用 waitpid(child_pid, &status) 后，内核找到对应的 Zombie 子进程，把 exit_status 写入 status 指针，把子进程从 children 链表中 unlink，释放 PID，把子进程状态改为 Dead。如果不调用 waitpid，Zombie 会一直占用 TCB 和 PID——这就是所谓的"zombie 进程"问题。

## 动手实现

### Step 1: ExecveResult 枚举定义

**目标**: 定义 execve 可能返回的所有错误码。

**设计思路**: execve 的错误来源很多——路径无效、文件不存在、不是普通文件、ELF 验证失败、内存分配失败等。每个错误需要一个独立的枚举值，值和 Linux errno 对齐，这样 syscall 层可以直接返回给用户态。

**实现约束**: 定义 scoped enum `ExecveResult`，放在 `cinux::proc` 命名空间。值包括 Ok=0、BadPath=-22(EINVAL)、FileNotFound=-2(ENOENT)、FileNotRegular=-21(EISDIR)、ReadFailed=-5(EIO)、BadElfMagic=-8(ENOEXEC) 等等。同时定义一个辅助函数 `elf_error_to_execve` 把 ElfValidateResult 转换成 ExecveResult。

### Step 2: execve() — VFS 路径解析

**目标**: 从 execve 的参数路径开始，通过 VFS 找到对应的 inode。

**设计思路**: execve 接收一个路径字符串（比如 "/bin/sh"）。首先做参数检查——路径不能是空指针也不能是空字符串。然后通过 vfs_resolve() 把路径解析到具体的文件系统实例和相对路径。接着用文件系统的 lookup() 方法找到 inode。最后检查 inode 的类型——必须是 Regular 文件，不能是目录。

**实现约束**: 用 vfs_resolve(path, &rel_path) 获取 fs 实例和相对路径。用 fs->lookup(rel_path) 获取 inode。inode->type 必须是 InodeType::Regular。任何一步返回 nullptr 都要返回对应的错误码（FileNotFound、FileNotRegular 等）。

**踩坑预警**: vfs_resolve 的第二个参数是输出参数。如果路径以 / 开头，rel_path 就是去掉前导 / 的部分（比如 "/bin/sh" -> "bin/sh"）。另外要先检查 inode->ops 是否为 nullptr——如果文件系统驱动没有实现 read 操作，直接返回 ReadFailed。

**验证**: execve 成功后串口打印 "[EXECVE] loaded /bin/sh entry=0x... pid=X"。如果失败会打印具体错误码，比如 "[EXECVE] path not found: /bin/nonexist"。

### Step 3: execve() — ELF 读取与验证

**目标**: 从 inode 读取 ELF header 和 program headers，完成验证。

**设计思路**: 先检查 inode->size >= sizeof(Elf64_Ehdr)，太小就返回 ReadFailed。然后分配 64 字节栈缓冲区读取 ELF header。验证合法后从中提取 program header table 的偏移和数量。然后用 aligned new 动态分配缓冲区一次性读取所有 program headers。用完记得 delete[] phdrs。

**实现约束**: 先 `if (inode->ops == nullptr) return ReadFailed` 检查 ops 非空。用 `inode->ops->read(inode, 0, buf, 64)` 读取 header。验证后用 `new (std::align_val_t{alignof(Elf64_Phdr)}) Elf64_Phdr[phnum]` 分配 program header 缓冲区，再用 `read(inode, phdr_offset, phdrs, phdr_bytes)` 读取。如果读取不足 phdr_bytes，delete[] phdrs 并返回 ReadFailed。

**踩坑预警**: inode->ops 可能为 nullptr——如果文件系统驱动没有实现 read 操作。一定要在读取前检查 ops 非空。另外 program headers 的字节数 `phdr_bytes = phnum * sizeof(Elf64_Phdr)`，需要和实际读取的字节数比较。

### Step 4: execve() — 清除旧映射并加载新段

**目标**: 清除当前地址空间的所有用户映射，加载 PT_LOAD 段。

**设计思路**: 清除旧映射需要遍历 PML4 的前 256 项，四层嵌套遍历到 PT 级别，释放每个数据页的物理内存，然后释放页表页本身（PT、PD、PDPT 页都逐一释放），将所有 entry 清零。加载新段时，对每个 PT_LOAD 段计算它覆盖的虚拟地址范围（seg_start = p_vaddr 向下对齐到页，seg_end = p_vaddr + p_memsz 向上对齐到页），逐页处理。每页分配一个新物理页，清零，然后计算这一页应该从 ELF 文件的哪个偏移读多少数据。

**实现约束**: 清除函数遍历 PML4[0..255]，对每个 present 的 entry 递归到 PT 级别，释放数据页和页表页的物理内存，将 entry 清零。加载时对每个 PT_LOAD 段，page_base_offset = vaddr - p_vaddr，如果 page_base_offset < p_filesz，从文件的 p_offset + copy_start 处读取 copy_len 字节到物理页的 copy_start 处。确定 page_flags 时根据 PF_W 设置 FLAG_WRITABLE，根据 PF_X 清除 FLAG_NX。

**验证**: execve 成功后串口打印 "[EXECVE] loaded /bin/sh entry=0x... pid=X"。如果失败会打印具体错误码。

### Step 5: execve() — 设置入口点

**目标**: 将 ELF header 中的入口点地址写入 task->ctx.rip。

**设计思路**: execve 的最后一步是设置 ctx.rip = ehdr->e_entry。注意此时我们还在内核态——execve 只负责准备地址空间和入口点，不负责跳转。跳转到用户态是调用者的责任（通过 jump_to_usermode）。同时检查 has_load_segment 标志——如果没有找到任何 PT_LOAD 段就返回 NoLoadSegments。

**实现约束**: 在所有 PT_LOAD 段加载完成后，检查 has_load_segment 标志。然后 `task->ctx.rip = ehdr->e_entry`，返回 ExecveResult::Ok。PID、ppid、parent、children 等进程身份信息保持不变（execve 只替换内存映像，不改变进程身份）。

### Step 6: waitpid() 实现

**目标**: 实现父进程等待子进程退出的系统调用。

**设计思路**: waitpid 扫描父进程的 children 链表找目标子进程。支持两种模式：pid > 0 时等待指定子进程，pid == -1 时等待任意一个退出的子进程。找到 Zombie 状态的子进程后，收集 exit_status、从链表 unlink、释放 PID、标记 Dead。

**实现约束**: 定义 scoped enum WaitpidResult（Ok=0, NoChildren=-10, NotFound=-3, InvalidPid=-22, NotExited=-1）。先检查 pid 参数的合法性（pid != -1 && pid <= 0 是无效的），然后检查父进程有没有子进程（children == nullptr 返回 NoChildren）。扫描链表时维护 prev 指针用于 unlink。如果子进程不是 Zombie 状态，返回 NotExited（非阻塞）。收集 status 到用户提供的指针（如果指针非空的话）。

**踩坑预警**: unlink 操作要处理目标在链表头的情况——如果 prev 是 nullptr，说明目标是头节点，要把 parent->children 改为 target->wait_next。释放 PID 后这个数字可以立即被新进程使用。

### Step 7: 系统调用注册

**目标**: 将 sys_fork、sys_execve、sys_waitpid 注册到系统调用分发表。

**实现约束**: 每个系统调用一个独立的 .hpp/.cpp 文件。sys_fork 调用 `cinux::proc::fork(cinux::proc::g_pid_alloc)` 并返回 child_pid。sys_execve 将参数从 uint64_t 转型为指针后调用 `cinux::proc::execve()`。sys_waitpid 调用 `cinux::proc::waitpid()`。三个函数都在 `kernel/arch/x86_64/syscall.cpp` 的分发表注册，对应 SyscallNr::SYS_fork=57, SYS_execve=59, SYS_waitpid=61。

## 构建与运行

```bash
cmake --build build
make run
```

如果跑内核测试：`make run-kernel-test`。

## 调试技巧

1. **execve 后 shell 不输出 prompt**: 检查 .rodata 段是否正确加载。在 shell 里按键盘如果能回显但 prompt 不显示，很可能是 ELF 段加载的偏移计算有问题——page_base_offset 的逻辑需要仔细检查。在 execve 加载每页后打印日志看数据是否正确读取。

2. **waitpid 找不到子进程**: 检查子进程是否被正确链入父进程的 children 列表。在 fork 的 Step 9 打印 parent->children 的 pid，确认链表正确。

3. **Zombie 进程堆积**: 如果 waitpid 永远返回 NoChildren，可能是子进程 exit 时没有正确设置 Zombie 状态。检查 sys_exit handler 是否把 state 设为 Zombie 并写入 exit_status。

4. **execve 报 ReadFailed**: 检查 inode->ops 是否为 nullptr，以及 inode->size 是否小于 ELF header 大小（64 字节）。文件可能不是一个有效的 ELF 文件。

## 本章小结

| 组件 | 关键设计 | 要点 |
|------|----------|------|
| ExecveResult | errno 风格错误码，值与 Linux 对齐 | BadPath=-22, FileNotFound=-2 等 |
| execve | VFS -> inode -> ELF -> clear -> load -> set entry | page_base_offset 计算段内偏移 |
| clear_user_mappings | 四层遍历释放所有用户页和数据页 | PML4[0..255]，释放 PT/PD/PDPT 页 |
| waitpid | 扫描 children 链表，找 Zombie，unlink | 非阻塞返回 NotExited |
| 进程状态机 | Ready -> Running -> Blocked/Zombie -> Dead | exit 设 Zombie, waitpid 设 Dead |
| sys_fork / sys_execve / sys_waitpid | 委托到 proc 命名空间 | syscall.cpp 分发表注册 |
