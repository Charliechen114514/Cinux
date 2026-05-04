# execve() 与 waitpid()

## 概览

本篇讲解 execve() 的 ELF 加载实现和 waitpid() 的子进程回收机制。execve 是进程生命周期中"替换"操作的核心——它把当前进程的地址空间清空，从文件系统加载一个新的 ELF 可执行文件。waitpid 是"回收"操作——父进程用它来收集子进程的退出状态并释放 TCB 资源。加上上一篇的 fork（"创建"），三者构成了完整的进程生命周期。

关键设计决策：execve 使用按页加载策略（逐页分配、清零、填充），而非一次性 mmap 整个文件；waitpid 是非阻塞的，只回收已经处于 Zombie 状态的子进程。

## 架构图

```
execve("/bin/sh"):
  VFS resolve ──→ inode lookup ──→ validate ELF header
      │                                  │
      ▼                                  ▼
  clear_user_mappings()          read program headers
      │                                  │
      ▼                                  ▼
  for each PT_LOAD segment:
    alloc phys page ──→ zero ──→ fill from file ──→ map
      │
      ▼
  task->ctx.rip = e_entry

waitpid(pid, &status):
  scan children list ──→ find Zombie ──→ collect status
      │                                       │
      ▼                                       ▼
  unlink from list                     free PID, mark Dead
```

## 代码精讲

### execve() — 路径解析与验证

```cpp
ExecveResult execve(const char* path, const char* const argv[],
                    const char* const envp[]) {
    (void)argv; (void)envp;  // 未来使用

    if (path == nullptr || path[0] == '\0')
        return ExecveResult::BadPath;

    auto* task = Scheduler::current();
    if (task == nullptr) return ExecveResult::NoCurrentTask;
    if (task->addr_space == nullptr) return ExecveResult::NoAddressSpace;

    const char* rel_path = nullptr;
    auto* fs = cinux::fs::vfs_resolve(path, &rel_path);
    if (fs == nullptr) return ExecveResult::FileNotFound;

    auto* inode = fs->lookup(rel_path);
    if (inode == nullptr) return ExecveResult::FileNotFound;
    if (inode->type != cinux::fs::InodeType::Regular)
        return ExecveResult::FileNotRegular;
```

execve 的前半段是纯验证：参数非空、任务存在、有地址空间、VFS 能解析路径、inode 存在、是普通文件。任何一步失败都返回对应的错误码，不继续执行。

### execve() — ELF 读取与验证

```cpp
    uint8_t ehdr_buf[sizeof(elf::Elf64_Ehdr)];
    if (inode->ops == nullptr) return ExecveResult::ReadFailed;

    int64_t nread = inode->ops->read(inode, 0, ehdr_buf, sizeof(elf::Elf64_Ehdr));
    if (nread < (int64_t)sizeof(elf::Elf64_Ehdr))
        return ExecveResult::ReadFailed;

    auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(ehdr_buf);
    auto vr = elf::validate_elf_header(ehdr, inode->size);
    if (vr != elf::ElfValidateResult::Ok)
        return elf_error_to_execve(vr);
```

ELF header 在栈上分配（64 字节），不需要堆内存。读取后立即验证。注意 inode->ops 检查——如果文件系统驱动没有实现 read 操作，直接返回错误。

### execve() — Program Header 读取

```cpp
    uint64_t phdr_bytes = ehdr->e_phnum * sizeof(elf::Elf64_Phdr);
    auto* phdrs = new (std::align_val_t{alignof(elf::Elf64_Phdr)}) elf::Elf64_Phdr[ehdr->e_phnum];
    nread = inode->ops->read(inode, ehdr->e_phoff, phdrs, phdr_bytes);
    if (nread < (int64_t)phdr_bytes) {
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }
```

Program headers 可能有很多个（虽然教学 shell 通常只有 2-3 个），所以用堆分配。用 aligned new 确保对齐要求（Elf64_Phdr 可能需要 8 字节对齐）。

### execve() — 清除旧映射

```cpp
void clear_user_mappings(cinux::mm::AddressSpace& space) {
    uint64_t pml4_phys = space.pml4_phys();
    auto* pml4 = reinterpret_cast<cinux::arch::PageEntry*>(pml4_phys + KERNEL_VMA);

    for (uint32_t i = 0; i < 256; i++) {
        if (!pml4[i].is_present()) continue;
        // ... 四层嵌套遍历到 PT 级别 ...
        // 释放数据页、PT 页、PD 页、PDPT 页
        // 每个 entry 清零
    }
}
```

这个函数做四层嵌套遍历：PML4 -> PDPT -> PD -> PT -> data page。释放所有用户空间的物理页（包括数据页和页表页本身），将所有 entry 清零。execve 后旧的地址空间被彻底清除，为新程序腾出位置。

### execve() — PT_LOAD 段加载（页内偏移修复版）

```cpp
    for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        auto* dst = reinterpret_cast<uint8_t*>(phys + KERNEL_VMA);
        for (uint64_t b = 0; b < PAGE_SIZE; b++) dst[b] = 0;  // 清零

        uint64_t data_vaddr  = (vaddr < phdr.p_vaddr) ? phdr.p_vaddr : vaddr;
        uint64_t in_page_off = data_vaddr - vaddr;
        uint64_t seg_offset  = data_vaddr - phdr.p_vaddr;

        if (seg_offset < phdr.p_filesz) {
            uint64_t copy_len = phdr.p_filesz - seg_offset;
            uint64_t avail    = PAGE_SIZE - in_page_off;
            if (copy_len > avail) copy_len = avail;

            inode->ops->read(inode, phdr.p_offset + seg_offset,
                             dst + in_page_off, copy_len);
        }

        task->addr_space->map(vaddr, phys, page_flags);
    }
```

页内偏移计算是这段代码的精髓。data_vaddr 取 vaddr 和 p_vaddr 的较大值——第一页时 p_vaddr 可能大于 vaddr（段起始非页对齐），后续页 data_vaddr = vaddr。in_page_off 是段数据在这一页内的起始位置——第一页可能非零，后续页都是 0。seg_offset 是段内偏移——用于计算文件读取位置。

举个例子：p_vaddr = 0x400260，那么 seg_start = 0x400000。第一页 vaddr = 0x400000，data_vaddr = max(0x400000, 0x400260) = 0x400260，in_page_off = 0x260，seg_offset = 0。写入物理页的 offset 0x260 处。第二页 vaddr = 0x401000，data_vaddr = max(0x401000, 0x400260) = 0x401000，in_page_off = 0，seg_offset = 0xDA0。写入物理页的 offset 0 处。

### execve() — 设置入口点

```cpp
    task->ctx.rip = ehdr->e_entry;
    return ExecveResult::Ok;
```

只改了 ctx.rip——入口点地址。PID、ppid、parent、children 等进程身份信息保持不变（execve 只替换内存映像，不改变进程身份）。

### waitpid() — 子进程扫描

```cpp
WaitpidResult waitpid(int pid, int* status, PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    if (pid != -1 && pid <= 0) return WaitpidResult::InvalidPid;
    if (parent->children == nullptr) return WaitpidResult::NoChildren;

    Task* target = nullptr;
    Task* prev = nullptr;

    if (pid == -1) {
        // 等待任意子进程：扫描找第一个 Zombie
        Task* cur = parent->children;
        while (cur != nullptr) {
            if (cur->state == TaskState::Zombie) { target = cur; break; }
            prev = cur; cur = cur->wait_next;
        }
    } else {
        // 等待指定子进程
        Task* cur = parent->children;
        while (cur != nullptr) {
            if (cur->pid == pid) { target = cur; break; }
            prev = cur; cur = cur->wait_next;
        }
    }
```

遍历 children 单链表时维护 prev 指针。pid == -1 时找第一个 Zombie（任意子进程），pid > 0 时找指定 PID 的子进程。如果 target 是 nullptr，说明没找到符合条件的子进程。

### waitpid() — 回收子进程

```cpp
    if (status != nullptr) *status = target->exit_status;

    if (prev != nullptr) prev->wait_next = target->wait_next;
    else parent->children = target->wait_next;

    pid_alloc.free(target->pid);
    target->state  = TaskState::Dead;
    target->parent = nullptr;

    return WaitpidResult::Ok;
```

四个操作：收集 exit_status、从链表 unlink、释放 PID、标记 Dead。unlink 要处理头节点的特殊情况——如果 prev 是 nullptr 说明 target 是链表头，要把 parent->children 指向下一个。释放 PID 后这个数字可以立即被新进程使用（PidAllocator 的 free 会拉回 hint）。

## 设计决策

### 决策：execve 是否在内核态跳转

**问题**: execve 应该在内部直接跳转到用户态，还是只设置入口点由调用者跳转？
**做法**: execve 只设 ctx.rip，调用者负责 jump_to_usermode。
**备选方案**: execve 内部完成地址空间切换和用户态跳转。
**原因**: 保持 execve 的职责单一——只负责 ELF 加载。跳转到用户态涉及设置用户栈、更新 TSS RSP0、更新 per-CPU syscall 栈等操作，这些和 ELF 加载逻辑无关。分离后 create_shell_terminal 和 kernel_init_thread 可以灵活控制跳转前的准备工作。

### 决策：waitpid 是否阻塞

**问题**: waitpid 应该阻塞等待子进程退出，还是非阻塞返回？
**做法**: 非阻塞——子进程没退出就返回 NotExited。
**备选方案**: 阻塞等待（让父进程 Blocked 直到子进程变成 Zombie）。
**原因**: Cinux 目前的调度器是 cooperative 的，阻塞 waitpid 需要实现"唤醒"机制（子进程 exit 时唤醒等待的父进程）。非阻塞版本更简单，Terminal 析构函数中用循环重试来模拟阻塞效果。

## 扩展方向

- 支持动态链接（PT_INTERP 段解析，加载 ld.so）
- execve 时传递 argv/envp 到用户栈（auxiliary vector）
- 阻塞版 waitpid（子进程 exit 时通过 wait queue 唤醒父进程）
- exit() 系统调用的完整实现（设置 Zombie、通知父进程）

## 参考资料
- Linux do_execve: https://github.com/torvalds/linux/blob/master/fs/exec.c
- OSDev Wiki ELF: https://wiki.osdev.org/ELF
- POSIX waitpid: https://pubs.opengroup.org/onlinepubs/9699919799/functions/waitpid.html
