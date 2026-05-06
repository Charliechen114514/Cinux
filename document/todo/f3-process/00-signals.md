# M1: 核心 POSIX 信号系统

> 从零构建信号系统。核心信号优先，抽象接口方便后续扩展实时信号。
> 实现 kill/signal/sigaction/sigprocmask 等系统调用。

## 目标

信号是进程间异步通知机制。实现：
1. 信号投递（内核 → 进程）
2. 信号处理（用户态 handler 调用）
3. 信号屏蔽（阻塞/解除阻塞）

## 任务清单

### T1: 信号定义与数据结构

**文件**: `kernel/proc/signal.hpp`

```cpp
namespace cinux::proc {

// 核心 POSIX 信号
enum class Signal : int {
    SIGHUP    = 1,    // 终端挂断
    SIGINT    = 2,    // 中断 (Ctrl+C)
    SIGQUIT   = 3,    // 退出 (Ctrl+\)
    SIGILL    = 4,    // 非法指令
    SIGTRAP   = 5,    // 断点陷阱
    SIGABRT   = 6,    // 异常终止
    SIGBUS    = 7,    // 总线错误
    SIGFPE    = 8,    // 浮点异常
    SIGKILL   = 9,    // 强制终止（不可捕获）
    SIGUSR1   = 10,   // 用户信号 1
    SIGSEGV   = 11,   // 段错误
    SIGUSR2   = 12,   // 用户信号 2
    SIGPIPE   = 13,   // 管道破裂
    SIGALRM   = 14,   // 定时器
    SIGTERM   = 15,   // 终止
    SIGCHLD   = 17,   // 子进程状态改变
    SIGCONT   = 18,   // 继续（不可捕获）
    SIGSTOP   = 19,   // 停止（不可捕获）
    SIGTSTP   = 20,   // 终端停止 (Ctrl+Z)
    SIGTTIN   = 21,   // 后台读
    SIGTTOU   = 22,   // 后台写
};

// 信号集（bitmask，64 位覆盖 1-64 号信号）
using SigSet = uint64_t;

// 默认动作
enum class SigDefault {
    Terminate,      // 终止进程
    CoreDump,       // 终止 + core dump
    Ignore,         // 忽略
    Stop,           // 停止进程
    Continue,       // 继续（如果已停止）
};

// 信号处理动作（抽象接口，方便扩展）
struct SigAction {
    enum class HandlerType : uint8_t {
        Default,    // SIG_DFL
        Ignore,     // SIG_IGN
        Custom,     // 用户 handler
    };

    HandlerType type;
    uint64_t    handler_addr;  // 用户态 handler 地址（type==Custom 时有效）
    SigSet      sa_mask;       // handler 执行期间屏蔽的信号集
    bool        sa_restart;    // SA_RESTART
    bool        sa_resethand;  // SA_RESETHAND（执行一次后重置为 SIG_DFL）
};

// 获取信号的默认动作
SigDefault signal_default_action(Signal sig);

// 不可捕获/忽略的信号
bool signal_is_uncatchable(Signal sig);

} // namespace cinux::proc
```

- [ ] Signal enum class（核心 22 个信号）
- [ ] SigSet bitmask 操作（sig_add/sig_del/sig_ismember）
- [ ] SigAction 结构体（抽象接口：Default/Ignore/Custom）
- [ ] signal_default_action() 查表
- [ ] signal_is_uncatchable() — SIGKILL/SIGSTOP

### T2: Task 结构体信号字段

**文件**: `kernel/proc/process.hpp`

在 Task 中添加信号相关字段：

```cpp
struct Task {
    // ... existing fields ...

    // 信号处理
    SigAction  sig_actions[32];   // 信号处理动作表（1-31，0 号未使用）
    SigSet     sig_pending;       // 待处理信号集
    SigSet     sig_blocked;       // 被屏蔽的信号集

    // 信号投递栈（用于用户态 handler 的 trampoline）
    uint64_t   sig_altstack;      // sigaltstack 地址（0 = 使用普通栈）
    uint64_t   sig_altstack_size;
};
```

- [ ] sig_actions[32] 数组
- [ ] sig_pending / sig_blocked bitmask
- [ ] sig_altstack 字段
- [ ] 构造时初始化：sig_pending = 0, sig_blocked = 0, 所有 action = Default

### T3: 信号投递机制

**文件**: `kernel/proc/signal.cpp`

```cpp
namespace cinux::proc {

// 向目标进程发送信号
int signal_send(Task* target, Signal sig);

// 检查并处理待处理信号（从 syscall/中断返回用户态前调用）
void signal_check_and_deliver(InterruptFrame* frame);

// 执行默认动作
void signal_exec_default(Task* task, Signal sig);

} // namespace cinux::proc
```

**signal_send 流程**:
1. 验证信号编号合法
2. 如果目标进程已 Zombie/Dead → 返回 ESRCH
3. 设置 target->sig_pending 的对应 bit
4. 如果目标进程 Blocked 且信号不是 SIGKILL/SIGSTOP → 等待 unblock
5. 如果目标进程 Blocked 但信号是 SIGKILL → 立即唤醒
6. 如果目标进程在 wait_queue 上 → 唤醒

**signal_check_and_deliver 流程**（关键路径）:
1. 取 pending & ~blocked 的交集（可投递信号）
2. 选最高优先级的信号（不可捕获 > 可捕获）
3. 从 pending 中清除该 bit
4. 查 sig_actions[sig]：
   - Default → signal_exec_default()
   - Ignore → 跳过
   - Custom → 构造 signal frame，修改用户态栈和 RIP 跳转到 handler
5. handler 返回后通过 sigreturn syscall 恢复原始上下文

- [ ] signal_send() 投递
- [ ] signal_check_and_deliver() 检查+投递
- [ ] signal_exec_default() 默认动作（终止/停止/忽略/继续）
- [ ] 在 syscall 返回路径调用 signal_check_and_deliver()

### T4: 用户态信号 Handler 调用

**文件**: `kernel/arch/x86_64/signal.S`（新增）

信号 handler 调用需要在用户态栈上构造 signal frame：

```
用户态栈布局（signal frame）:
┌───────────────────┐ ← 原 RSP
│  SignalFrame      │
│  ├─ rax           │
│  ├─ rbx           │
│  ├─ rcx           │
│  ├─ ...           │ (所有通用寄存器)
│  ├─ rflags        │
│  ├─ rip           │ (被中断的地址)
│  ├─ cs            │
│  ├─ rsp           │
│  └─ ss            │
├───────────────────┤
│  signal number    │
│  sigreturn addr   │ ← handler 返回地址（指向 sigreturn trampoline）
└───────────────────┘ ← 新 RSP
```

- [ ] SignalFrame 结构体定义
- [ ] 构造 signal frame 到用户栈
- [ ] 修改 RIP 指向用户 handler
- [ ] sigreturn trampoline（syscall sigreturn）

### T5: sigreturn 系统调用

**文件**: `kernel/syscall/sys_signal.cpp`

```cpp
int64_t sys_sigreturn();
```

- handler 返回后调用 sigreturn，从 signal frame 恢复原始上下文
- [ ] 从用户栈读取 SignalFrame
- [ ] 恢复所有寄存器 + RIP + RFLAGS
- [ ] 恢复 sig_blocked（handler 期间的临时屏蔽）

### T6: 信号相关 Syscall

**文件**: `kernel/syscall/sys_signal.cpp`

| Syscall | 编号 | 说明 |
|---------|------|------|
| sys_kill | 62 | 向进程发送信号 |
| sys_rt_sigaction | 13 | 设置/查询信号处理动作 |
| sys_rt_sigprocmask | 14 | 设置/查询信号屏蔽字 |
| sys_rt_sigreturn | 15 | 从信号 handler 返回 |

- [ ] sys_kill(pid, sig)
- [ ] sys_rt_sigaction(sig, act, oact)
- [ ] sys_rt_sigprocmask(how, set, oset)
- [ ] sys_rt_sigreturn()
- [ ] 注册到 syscall 表
- [ ] libc wrapper

### T7: 集成点

- [ ] sys_read/write 返回 -EPIPE 时发送 SIGPIPE
- [ ] 非法内存访问（PF handler）发送 SIGSEGV 替代 panic
- [ ] 非法指令异常发送 SIGILL
- [ ] 子进程退出时向父进程发送 SIGCHLD
- [ ] sys_waitpid 被中断时检查 SIGCHLD

### T8: 单元测试

- [ ] signal_send 设置 pending bit
- [ ] signal_check_and_deliver 投递到 handler
- [ ] sigreturn 恢复原始上下文
- [ ] 信号屏蔽生效（blocked 信号不投递）
- [ ] SIGKILL 不可屏蔽/不可捕获
- [ ] 默认动作：终止/忽略

## 产出物

- [ ] `kernel/proc/signal.hpp` — Signal enum + SigAction + 接口
- [ ] `kernel/proc/signal.cpp` — 投递/检查/默认动作
- [ ] `kernel/arch/x86_64/signal.S` — signal frame + sigreturn trampoline
- [ ] `kernel/syscall/sys_signal.cpp` — kill/sigaction/sigprocmask/sigreturn
- [ ] `kernel/proc/process.hpp` — Task 信号字段
- [ ] 集成到 PF handler + syscall 返回路径
- [ ] libc 信号 wrapper
- [ ] 单元测试
