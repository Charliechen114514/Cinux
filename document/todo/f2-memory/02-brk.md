# M3: brk Syscall + 用户态堆

> 实现 brk() 系统调用，为用户程序提供动态内存分配基础。
> brk 是 sbrk/malloc 的底层支撑。

## 目标

Linux syscall 12: `brk()` — 调整进程数据段末尾（program break）。
这是用户态 malloc 实现的最简接口。

## 现有代码

- M1 产出的 VMA 管理（Heap flag VMA）
- M2 产出的 mmap（如果 brk 在 mmap 之后实现，可以复用 VMA 基础设施）
- `kernel/arch/x86_64/memory_layout.hpp` — `USER_BRK_BASE`, `USER_BRK_MAX`
- `kernel/proc/process.hpp` — Task 结构体

## 任务清单

### T1: Task 结构体扩展

**文件**: `kernel/proc/process.hpp`

在 Task 中添加 brk 相关字段：

```cpp
struct Task {
    // ... existing fields ...
    uint64_t brk_current;    // 当前 program break（页对齐）
    uint64_t brk_initial;    // 初始 brk（ELF 加载完成后设置）
    uint64_t brk_max;        // brk 上限
};
```

- [ ] 添加 brk_current / brk_initial / brk_max 字段
- [ ] 初始值：brk_current = brk_initial = USER_BRK_BASE（或 ELF 段末尾）

### T2: brk 系统调用实现

**文件**: `kernel/syscall/sys_brk.cpp`

```cpp
int64_t sys_brk(uint64_t addr);
```

**语义**（Linux 行为）:
- `addr == 0` → 返回当前 brk（不修改）
- `addr < brk_initial` → 忽略，返回当前 brk
- `addr > brk_max` → 忽略，返回当前 brk
- 正常情况：设置 brk_current = addr，返回新 brk

**实现流程**:
1. 获取当前 Task
2. 如果 addr == 0，返回 brk_current
3. 边界检查：brk_initial ≤ addr ≤ brk_max
4. 如果 addr > brk_current（扩展堆）:
   - 为 [brk_current, addr) 分配物理页
   - 映射到进程地址空间（FLAG_USER | FLAG_WRITABLE）
   - 更新或创建 Heap VMA
5. 如果 addr < brk_current（收缩堆）:
   - 释放 [addr, brk_current) 的物理页
   - 取消映射
   - 更新 Heap VMA
6. 设置 brk_current = addr
7. 返回新的 brk_current

- [ ] 注册 syscall 12 (SYS_brk)
- [ ] sys_brk 主逻辑
- [ ] 堆扩展：分配页 + 映射 + VMA 更新
- [ ] 堆收缩：释放页 + 取消映射 + VMA 更新
- [ ] libc 添加 brk() 和 sbrk() wrapper

### T3: execve 中设置初始 brk

**文件**: `kernel/proc/process.cpp`

- [ ] execve 加载 ELF 后，根据最高 PT_LOAD 段的末尾地址设置 brk_initial
- [ ] brk_current = brk_initial
- [ ] brk_max = USER_BRK_MAX（或 brk_initial + 256MB）
- [ ] 创建初始 Heap VMA [brk_initial, brk_initial)

### T4: 用户态 malloc 测试

**文件**: `user/test_brk.c`（新增用户态测试程序）

- [ ] 调用 sbrk(0) 获取当前 break
- [ ] 调用 sbrk(4096) 扩展堆
- [ ] 写入扩展区域验证可访问
- [ ] 调用 sbrk(-4096) 收缩堆
- [ ] 基于此实现一个最简 bump allocator

## 产出物

- [ ] `kernel/syscall/sys_brk.cpp` — brk 系统调用
- [ ] `kernel/proc/process.hpp` — Task brk 字段
- [ ] `kernel/proc/process.cpp` — execve brk 初始化
- [ ] `kernel/syscall/syscall_nums.hpp` — SYS_brk 编号
- [ ] libc brk/sbrk wrapper
- [ ] `user/test_brk.c` — 用户态测试
- [ ] 编译通过 + brk 测试程序在 QEMU 运行
