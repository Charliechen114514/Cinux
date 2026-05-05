# M5: Demand Paging 增强

> 增强 page fault handler，支持 VMA 验证的按需分页。
> 匿名映射零填充，文件映射从 Page Cache 填充。

## 目标

让 mmap 创建的映射在首次访问时才分配物理页，实现真正的 demand paging。

## 依赖

- M1: VMA 管理（查找合法映射区域）
- M4: Page Cache（文件映射的数据来源）

## 现有代码

- `kernel/arch/x86_64/exception_handlers.cpp:167-276` — 当前 PF handler
  - 已有：基本 demand paging（零填充，无 VMA 检查）
  - 已有：CoW fault 处理
  - 已有：guard page 检测
- M1 产出的 VMA 查找
- M4 产出的 Page Cache

## 任务清单

### T1: 重构 Page Fault Handler

**文件**: `kernel/arch/x86_64/exception_handlers.cpp`

新的 PF handler 流程：

```
page_fault(frame):
    fault_addr = read_cr2()
    error_code = frame->error_code

    // 1. Guard page 检测（保留现有逻辑）
    if is_guard_page(fault_addr):
        panic("stack overflow")

    // 2. 获取当前进程的 VMA store
    task = Scheduler::current()
    if !task || !task->addr_space:
        panic("PF in kernel context")

    // 3. VMA 查找
    vma = task->addr_space->vmas().find(fault_addr)
    if !vma:
        // 非法访问 → kill 进程
        kill_process(task, SIGSEGV)  // 或暂时 panic + 详细信息
        return

    // 4. 权限检查
    if (write_fault && !has_flag(vma->flags, VmaFlags::Write)):
        kill_process(task, SIGSEGV)
        return
    if (exec_fault && !has_flag(vma->flags, VmaFlags::Exec)):
        kill_process(task, SIGSEGV)
        return

    // 5. CoW 处理（保留现有逻辑）
    if (present && write && cow_bit):
        handle_cow_fault(fault_addr)
        return

    // 6. Demand paging
    if (!present):
        page_addr = fault_addr & ~0xFFFULL
        if has_flag(vma->flags, VmaFlags::Anonymous):
            handle_anonymous_fault(task, vma, page_addr)
        elif vma->backing_inode:
            handle_file_fault(task, vma, page_addr)
        return
```

- [ ] 重构 handle_pf() 为主分发函数
- [ ] VMA 查找验证（替换现有的无检查 demand paging）
- [ ] 非法访问 → kill 进程（而非 panic）
- [ ] 权限不匹配 → kill 进程
- [ ] 保留 guard page 和 CoW 逻辑

### T2: 匿名页错误处理

```cpp
void handle_anonymous_fault(Task* task, VMA* vma, uint64_t page_addr) {
    // 分配物理页
    uint64_t phys = g_pmm.alloc_page();
    if (!phys) {
        // OOM → kill 进程
        kill_process(task, SIGKILL);
        return;
    }

    // 零填充（安全）
    void* virt = (void*)(phys + KERNEL_VMA);
    memset(virt, 0, PAGE_SIZE);

    // 映射到进程地址空间
    uint64_t flags = FLAG_PRESENT | FLAG_USER;
    if (has_flag(vma->flags, VmaFlags::Write))
        flags |= FLAG_WRITABLE;
    if (!has_flag(vma->flags, VmaFlags::Exec))
        flags |= FLAG_NX;

    task->addr_space->map(page_addr, phys, flags);
}
```

- [ ] 零填充页分配
- [ ] 根据 VMA 权限设置页表标志
- [ ] OOM 处理

### T3: 文件页错误处理

```cpp
void handle_file_fault(Task* task, VMA* vma, uint64_t page_addr) {
    // 计算文件偏移
    uint64_t file_offset = vma->file_offset + (page_addr - vma->start);

    // 从 Page Cache 获取页
    CachedPage* cp = g_page_cache.get_page(vma->backing_inode, file_offset);
    if (!cp) {
        kill_process(task, SIGBUS);
        return;
    }

    // 映射到进程地址空间
    uint64_t flags = FLAG_PRESENT | FLAG_USER;
    if (has_flag(vma->flags, VmaFlags::Write))
        flags |= FLAG_WRITABLE;
    if (!has_flag(vma->flags, VmaFlags::Exec))
        flags |= FLAG_NX;

    // MAP_PRIVATE: CoW 语义（先映射为只读 + COW 标记）
    if (!has_flag(vma->flags, VmaFlags::Shared)) {
        flags &= ~FLAG_WRITABLE;
        flags |= FLAG_COW;
    }

    task->addr_space->map(page_addr, cp->page_phys, flags);
}
```

- [ ] 文件偏移计算
- [ ] Page Cache 集成
- [ ] MAP_PRIVATE CoW 语义
- [ ] MAP_SHARED 直接映射

### T4: mmap 懒分配（对接 M2）

M2 的 sys_mmap 创建 VMA 时不分配物理页。本 milestone 使其生效：

- [ ] 确认 mmap 只创建 VMA、不分配物理页
- [ ] 首次访问触发 PF → 匿名/文件 fault handler 处理
- [ ] 连续页的 demand paging 正确（多次 PF 逐页分配）

### T5: 单元测试

**文件**: `kernel/test/test_demand_paging.cpp`

- [ ] 匿名 VMA 首次写入触发 demand paging
- [ ] 文件 VMA 首次读取从 Page Cache 填充
- [ ] CoW 写入触发页面复制
- [ ] 非法地址访问被 kill
- [ ] 权限违规被 kill（写只读 VMA）
- [ ] 大区域 mmap 懒分配（不立即占满物理内存）

## 产出物

- [ ] `kernel/arch/x86_64/exception_handlers.cpp` — 重构的 PF handler
- [ ] 匿名 demand paging
- [ ] 文件 demand paging（Page Cache 集成）
- [ ] 进程 kill 机制（非法访问）
- [ ] `kernel/test/test_demand_paging.cpp` — 单元测试
- [ ] QEMU 运行：用户程序 mmap + 访问正常
