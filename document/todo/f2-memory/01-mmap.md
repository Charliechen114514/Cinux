# M2: mmap/munmap/mprotect Syscall

> 基于 M1 的 VMA 管理，实现 mmap 系统调用族。
> 支持匿名映射、文件映射（基础）、权限修改。

## 目标

实现 Linux 风格的 mmap/munmap/mprotect，使 Cinux 用户程序可以使用动态内存映射。

## 现有代码

- M1 产出的 VMA 管理层
- `kernel/mm/vmm.hpp` — map/unmap 页表操作
- `kernel/mm/pmm.hpp` — alloc_page 物理页分配
- `kernel/fs/inode.hpp` — Inode + InodeOps
- `kernel/arch/x86_64/exception_handlers.cpp` — Page fault handler（需增强）

## 任务清单

### T1: mmap 系统调用

**文件**: `kernel/syscall/sys_mmap.cpp`

Linux x86_64 syscall 9: `void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)`

```cpp
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                 uint64_t flags, uint64_t fd, uint64_t offset);
```

**支持的 flags**:
- `MAP_PRIVATE` (0x02) — CoW 映射
- `MAP_ANONYMOUS` (0x20) — 无文件 backing
- `MAP_FIXED` (0x10) — 强制指定地址
- `MAP_SHARED` (0x01) — 共享映射（基础支持）

**支持的 prot**:
- `PROT_READ` (0x1), `PROT_WRITE` (0x2), `PROT_EXEC` (0x4)

**实现流程**:
1. 参数验证：length > 0、offset 页对齐、prot 合法组合
2. 页对齐 length 向上取整
3. 如果 MAP_FIXED：验证 [addr, addr+length) 与现有 VMA 无冲突或替换
4. 否则：调用 `find_free_area()` 寻找空闲区域
5. 创建 VMA 并插入 VMA store
6. **懒分配**：不立即映射物理页，仅建立 VMA 记录
7. page fault 时再实际分配（M5 Demand Paging 增强）
8. 如果是文件映射：记录 backing_inode + offset
9. 返回映射起始地址

- [ ] 注册 syscall 9 (SYS_mmap) 到 syscall 表
- [ ] 实现 sys_mmap 主逻辑
- [ ] 匿名映射（MAP_ANONYMOUS）
- [ ] MAP_FIXED 地址指定
- [ ] MAP_PRIVATE CoW 语义（只创建 VMA，不立即分配）
- [ ] 文件映射（从 fd → inode → VMA backing_inode）
- [ ] libc 添加 mmap() wrapper

### T2: munmap 系统调用

**文件**: `kernel/syscall/sys_mmap.cpp`（同文件）

Linux syscall 11: `int munmap(void* addr, size_t length)`

```cpp
int64_t sys_munmap(uint64_t addr, uint64_t length);
```

**实现流程**:
1. 验证 addr 页对齐、length > 0
2. 在 VMA store 中查找覆盖 [addr, addr+length) 的 VMA
3. 如果 VMA 边界不完全匹配：拆分 VMA（remove 部分）
4. 释放覆盖范围内的物理页：`g_pmm.free_page()`
5. 取消页表映射：`g_vmm.unmap()`（需遍历所有涉及的页）
6. 如果是文件映射 + dirty：标记 inode 需要写回
7. 返回 0 成功

- [ ] 注册 syscall 11 (SYS_munmap)
- [ ] 实现 sys_munmap
- [ ] 正确拆分部分覆盖的 VMA
- [ ] 释放物理页 + 取消映射
- [ ] libc 添加 munmap() wrapper

### T3: mprotect 系统调用

**文件**: `kernel/syscall/sys_mmap.cpp`（同文件）

Linux syscall 10: `int mprotect(void* addr, size_t len, int prot)`

```cpp
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
```

**实现流程**:
1. 验证 addr 页对齐、len > 0
2. 查找覆盖的 VMA
3. 更新 VMA 的 flags
4. 遍历范围内所有已映射的页，更新 PTE 权限位
5. 刷新 TLB

- [ ] 注册 syscall 10 (SYS_mprotect)
- [ ] 修改已映射页的权限位
- [ ] 正确处理拆分（前后保持原权限）
- [ ] libc 添加 mprotect() wrapper

### T4: Page Fault Handler 增强（VMA 验证）

**文件**: `kernel/arch/x86_64/exception_handlers.cpp`

当前 page fault handler 的 demand paging 不检查 VMA。需要：

1. 在 demand paging 分支中查找当前进程的 VMA
2. 如果地址不在任何 VMA 内 → 发送 SIGSEGV（或当前直接 panic → 改为 kill 进程）
3. 如果地址在 VMA 内 → 按权限分配页：
   - 匿名 VMA → 零填充页
   - 文件 VMA → 从 page cache 读取（M5 实现）
   - CoW VMA → 复制物理页
4. 检查 fault 类型与 VMA 权限匹配：
   - 写 fault → VMA 必须有 Write flag
   - 执行 fault → VMA 必须有 Exec flag

- [ ] Page fault 时查询 VMA store
- [ ] 非法访问 → kill 进程（而非 panic）
- [ ] 匿名 VMA demand paging
- [ ] 权限不匹配 → kill 进程
- [ ] 保留现有 CoW 和 guard page 逻辑

### T5: execve 改造（注册 VMA）

**文件**: `kernel/proc/process.cpp`

当前 execve 直接操作页表，不记录 VMA。需要同步注册：

- [ ] 每个 PT_LOAD 段映射后，创建对应 VMA（权限由 p_flags 决定）
- [ ] 用户栈映射后，创建 Stack VMA
- [ ] `clear_user_mappings()` 改为同时清理 VMA store

### T6: fork 改造（复制 VMA）

**文件**: `kernel/proc/process.cpp`

当前 fork 复制页表（CoW），但不复制 VMA 记录：

- [ ] fork 时复制父进程的所有 VMA 到子进程的 VMA store
- [ ] CoW VMA 正确标记 MAP_PRIVATE
- [ ] 共享映射（MAP_SHARED）正确共享

### T7: 单元测试

**文件**: `kernel/test/test_mmap.cpp`

- [ ] 匿名 mmap 分配 + 读写
- [ ] munmap 释放 + 再分配
- [ ] MAP_FIXED 指定地址
- [ ] mprotect 修改权限
- [ ] VMA 部分释放拆分正确
- [ ] 非法访问被 kill（非 panic）
- [ ] fork 后 CoW 独立（写时不共享）

## 产出物

- [ ] `kernel/syscall/sys_mmap.cpp` — mmap/munmap/mprotect 实现
- [ ] `kernel/arch/x86_64/exception_handlers.cpp` — PF 增强
- [ ] `kernel/proc/process.cpp` — execve/fork VMA 注册
- [ ] `kernel/syscall/syscall_nums.hpp` — 新增 3 个 syscall 编号
- [ ] libc mmap/munmap/mprotect wrapper
- [ ] `kernel/test/test_mmap.cpp` — 单元测试
- [ ] 编译通过 + QEMU mmap 测试程序运行
