# M2: clone + futex + TLS（线程支持）

> Linux 风格线程：clone() 创建共享地址空间的轻量进程。
> futex 实现高效用户态互斥，TLS 支持线程局部存储。

## 目标

为 musl pthread 实现提供内核基础：
- clone()：线程创建原语
- futex()：用户态快速互斥锁
- TLS：线程局部存储（fs/gs segment）

## 依赖

- M1 信号系统（clone 需要继承信号 mask）
- F2 mmap（线程栈分配）

## 现有代码

- `kernel/proc/process.cpp` — fork 实现（可参考改造为 clone）
- `kernel/arch/x86_64/context_switch.S` — 上下文切换
- `kernel/arch/x86_64/usermode.S` — 用户态入口
- `kernel/proc/sync.hpp` — 内核 Mutex/Semaphore（参考等待队列模式）

## 任务清单

### T1: clone 系统调用

**文件**: `kernel/syscall/sys_clone.cpp`

Linux syscall 56: `long clone(unsigned long flags, void* stack, int* parent_tid, int* child_tid, unsigned long tls)`

```cpp
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                  uint64_t child_tid, uint64_t tls);
```

**核心 flags**:
| Flag | 值 | 说明 |
|------|---|------|
| CLONE_VM | 0x100 | 共享地址空间（线程核心标志） |
| CLONE_FS | 0x200 | 共享文件系统信息（cwd） |
| CLONE_FILES | 0x400 | 共享文件描述符表 |
| CLONE_SIGHAND | 0x800 | 共享信号处理动作 |
| CLONE_THREAD | 0x10000 | 同一线程组 |
| CLONE_PARENT_SETTID | 0x100000 | 写 child_tid |
| CLONE_CHILD_CLEARTID | 0x200000 | child 退出时清 child_tid |
| CLONE_SETTLS | 0x40000 | 设置 TLS 段 |

**实现流程**:
1. 分配新 Task
2. 根据 flags 决定资源共享：
   - CLONE_VM → 共享 addr_space（引用计数 +1）
   - CLONE_FILES → 共享 fd_table
   - CLONE_SIGHAND → 共享 sig_actions
   - CLONE_FS → 共享 cwd
3. 如果 stack != 0 → 使用指定栈（线程栈）
4. 如果 CLONE_SETTLS → 设置 fs/gs 段基址
5. 复制页表（不共享的部分走 CoW）
6. 设置子进程返回值为 0
7. 唤醒子进程
8. 返回子进程 TID

- [ ] 定义 CLONE_* 常量
- [ ] sys_clone 主逻辑
- [ ] 地址空间共享（CLONE_VM）
- [ ] fd_table 共享（CLONE_FILES）
- [ ] 信号 action 共享（CLONE_SIGHAND）
- [ ] 自定义线程栈
- [ ] 注册 syscall 56
- [ ] libc clone() wrapper

### T2: futex 系统调用

**文件**: `kernel/syscall/sys_futex.cpp`

Linux syscall 202: `long futex(int* uaddr, int op, int val, const struct timespec* timeout, int* uaddr2, int val3)`

初期支持的操作：
| Op | 说明 |
|----|------|
| FUTEX_WAIT | 如果 *uaddr == val 则睡眠等待 |
| FUTEX_WAKE | 唤醒最多 val 个等待者 |
| FUTEX_WAIT_BITSET | 带 bitmask 的等待 |
| FUTEX_WAKE_BITSET | 带 bitmask 的唤醒 |

**内核侧数据结构**:
```cpp
struct FutexBucket {
    Spinlock lock;
    struct Waiter {
        Task*    task;
        uint32_t bitset;
        Waiter*  next;
    };
    Waiter* head;
};

// 全局 futex 哈希表（按 uaddr 虚拟地址分桶）
static FutexBucket futex_table[FUTEX_BUCKETS];
```

**FUTEX_WAIT 流程**:
1. 从用户空间读取 *uaddr
2. 如果 *uaddr != val → 立即返回 EAGAIN
3. 在 futex_table[hash(uaddr)] 中挂入等待队列
4. 阻塞当前线程：Scheduler::block()
5. 被唤醒后从等待队列移除，返回 0

**FUTEX_WAKE 流程**:
1. 在 futex_table[hash(uaddr)] 中查找等待者
2. 唤醒最多 val 个等待者：Scheduler::unblock()
3. 返回实际唤醒数

- [ ] FutexBucket 哈希表（256 桶）
- [ ] FUTEX_WAIT 实现
- [ ] FUTEX_WAKE 实现
- [ ] 超时支持（timespec → tick 比较）
- [ ] 注册 syscall 202
- [ ] libc futex() wrapper

### T3: TLS（Thread Local Storage）

**文件**: `kernel/arch/x86_64/tls.hpp`, `kernel/arch/x86_64/tls.cpp`

x86_64 使用 FS 段基址实现 TLS。每个线程有独立的 FS 基址指向 TLS 块。

```cpp
namespace cinux::arch {

// 设置当前线程的 TLS 基址
void set_tls_base(uint64_t addr);

// 获取当前线程的 TLS 基址
uint64_t get_tls_base();

} // namespace cinux::arch
```

**实现**:
- `set_tls_base()` → 写入 MSR_FS_BASE (0xC0000100)
- clone(CLONE_SETTLS) 时调用
- 上下文切换时保存/恢复 FS_BASE

- [ ] set_tls_base() / get_tls_base() 实现
- [ ] 上下文切换中保存/恢复 FS_BASE
- [ ] clone CLONE_SETTLS 调用 set_tls_base()

### T4: 上下文切换增强

**文件**: `kernel/arch/x86_64/context_switch.S`

当前只保存 callee-saved 寄存器。线程需要额外保存：

- [ ] CpuContext 增加 fs_base 字段
- [ ] SAVE 阶段读取 MSR_FS_BASE 保存到 context
- [ ] RESTORE 阶段写回 MSR_FS_BASE

### T5: 线程组与 TID

**文件**: `kernel/proc/process.hpp`

```cpp
struct Task {
    // ... existing ...
    int    tgid;          // 线程组 ID (= 主线程 PID)
    Task*  group_leader;  // 线程组 leader
    int*   clear_child_tid; // CLONE_CHILD_CLEARTID 地址
};
```

- [ ] tgid 字段（getpid() 返回 tgid 而非 pid）
- [ ] group_leader 指针
- [ ] CLONE_CHILD_CLEARTID：exit 时写入 0 到 clear_child_tid + futex_wake

### T6: 单元测试

- [ ] clone(CLONE_VM) 创建线程共享地址空间
- [ ] clone 指定栈地址
- [ ] futex_wait + futex_wake 唤醒
- [ ] futex_wait *uaddr != val 立即返回
- [ ] TLS 设置/读取正确
- [ ] 多线程共享 fd_table 读写

## 产出物

- [ ] `kernel/syscall/sys_clone.cpp` — clone 系统调用
- [ ] `kernel/syscall/sys_futex.cpp` — futex 系统调用
- [ ] `kernel/arch/x86_64/tls.hpp` / `.cpp` — TLS 支持
- [ ] `kernel/arch/x86_64/context_switch.S` — 增强 FS_BASE 保存
- [ ] `kernel/proc/process.hpp` — 线程组字段
- [ ] libc clone/futex wrapper
- [ ] 单元测试
