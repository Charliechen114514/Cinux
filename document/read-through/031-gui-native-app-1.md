# 031-1: Pipe IPC — 环形缓冲区、VFS 适配与 pipe 系统调用

## 概览

本文是 tag 031 (gui_native_app) 通读系列的第一篇，聚焦于管道 IPC 的完整实现。在整个 tag 中，pipe 是终端与 shell 之间通信的核心基础设施——它让字节数据能从键盘输入流到 shell 进程，再从 shell 的输出流回到终端显示。

我们将依次讲解三组源文件：`pipe.hpp/cpp`（环形缓冲区数据结构）、`pipe_ops.hpp/cpp`（VFS 适配器）、`sys_pipe.hpp/cpp`（系统调用）。代码来自 `kernel/ipc/` 和 `kernel/syscall/` 目录。

关键设计决策：Cinux 采用 head/tail/count 三变量管理环形缓冲区（而非 xv6 的单调递增计数器），使用自旋锁保护，阻塞等待采用 spin-wait + hlt 而非 sleep/wakeup。

## 架构图

```
User Process                 Kernel
-----------                  -------
                             +-- FDTable --+
                             | fd[0] -> File(read_inode, RDONLY)
 pipe()  ------------------> | fd[1] -> File(write_inode, WRONLY)
                             +-------------+
                                    |           |
                              +-----+     +-----+
                              v                 v
                        PipeReadOps       PipeWriteOps
                              |                 |
                              +-------+---------+
                                      |
                                      v
                                  Pipe (ring buffer)
                                  [head, tail, count, lock]
```

## 代码精讲

### Pipe 数据结构定义 (pipe.hpp)

```cpp
static constexpr uint32_t PIPE_BUFFER_SIZE = 4096;
static constexpr uint32_t PIPE_SPIN_WAIT_ITERS = 1'000'000;

class Pipe {
public:
    Pipe();
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    int64_t write(const char* data, uint64_t count);
    int64_t read(char* buf, uint64_t count);
    void close_reader();
    void close_writer();

    bool reader_alive() const;
    bool writer_alive() const;
    bool is_empty() const;
    bool is_full() const;
    uint32_t available() const;

    int64_t try_read(char* buf, uint64_t count);
    int64_t try_write(const char* data, uint64_t count);

private:
    char buffer_[PIPE_BUFFER_SIZE];
    uint32_t head_;
    uint32_t tail_;
    uint32_t count_;
    bool reader_open_;
    bool writer_open_;
    cinux::proc::Spinlock lock_;
};
```

缓冲区大小选了 4096 字节（一页），和 Linux 的默认管道大小一致。`head_` 是下一个要读取的位置，`tail_` 是下一个要写入的位置，`count_` 是当前缓冲区中的字节数。三个变量联合起来完全描述了环形缓冲区的状态——`count_ == 0` 表示空，`count_ == PIPE_BUFFER_SIZE` 表示满。

管道是禁用拷贝的（delete 拷贝构造和赋值），因为它内含自旋锁和原始缓冲区，拷贝语义没有意义。

### Pipe 构造函数 (pipe.cpp)

```cpp
Pipe::Pipe()
    : head_(0), tail_(0), count_(0),
      reader_open_(true), writer_open_(true) {}
```

构造时所有状态清零，两端默认打开。管道创建后处于"空"状态——reader 和 writer 都在等待对方先动。

### 阻塞写入 (pipe.cpp — Pipe::write)

写入是管道最复杂的部分，我们需要处理缓冲区满时的等待、跨数组末尾的两段拷贝、以及对端关闭的检测。

```cpp
int64_t Pipe::write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) return -1;
    uint64_t orig_flags = irq_save();
    uint64_t written = 0;

    while (written < count) {
        lock_.acquire();
        if (!reader_open_) { lock_.release(); goto out; }

        if (count_ == PIPE_BUFFER_SIZE) {
            lock_.release();
            irq_enable();
            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                hlt();
                irq_disable();
                lock_.acquire();
                bool still_full  = (count_ == PIPE_BUFFER_SIZE);
                bool reader_gone = !reader_open_;
                lock_.release();
                if (!still_full || reader_gone) break;
                irq_enable();
            }
            irq_disable();
            continue;
        }
        // ... data copy ...
    }
out:
    irq_restore(orig_flags);
    return (written > 0) ? static_cast<int64_t>(written)
                         : (reader_open_ ? 0 : -1);
}
```

这里有一个非常关键的 IRQ 安全设计。进入函数时先 `irq_save()` 保存当前中断标志并关中断，然后获取自旋锁。如果缓冲区满需要等待，必须先释放锁再启用中断——因为 PIT 中断处理程序会调用 `poll_output -> try_read`，而 try_read 也需要获取同一把锁。如果我们在持锁且关中断的状态下去 hlt，PIT 中断进不来，try_read 永远执行不了，缓冲区永远不满，死锁。

spin-wait 循环内部的流程是：hlt 让 CPU 休息 -> 关中断 -> 获取锁检查条件 -> 释放锁 -> 如果条件满足就跳出。每个循环末尾重新启用中断，让其他中断也有机会处理。

数据拷贝部分处理了两段写入——当 tail_ 到了数组末尾但还没写完时：

```cpp
uint32_t space = PIPE_BUFFER_SIZE - count_;
uint64_t chunk = count - written;
if (chunk > space) chunk = space;

uint32_t first = PIPE_BUFFER_SIZE - tail_;
if (first > chunk) first = static_cast<uint32_t>(chunk);

for (uint32_t i = 0; i < first; i++)
    buffer_[tail_ + i] = data[written + i];
tail_ = (tail_ + first) % PIPE_BUFFER_SIZE;
count_ += first;
written += first;

uint32_t second = static_cast<uint32_t>(chunk) - first;
if (second > 0) {
    for (uint32_t i = 0; i < second; i++)
        buffer_[tail_ + i] = data[written + i];
    tail_ = (tail_ + second) % PIPE_BUFFER_SIZE;
    count_ += second;
    written += second;
}
```

`first` 是从 tail_ 到数组末尾能写入的字节数，`second` 是回绕到数组开头后剩余的字节数。如果一次写入的数据没有跨越数组末尾，`second` 就是 0。

### 阻塞读取 (pipe.cpp — Pipe::read)

read 的结构和 write 对称，处理缓冲区空时的等待、跨数组末尾的两段拷贝、以及写端关闭后缓冲区耗尽的 EOF 检测：

```cpp
// Writer closed and buffer drained -- EOF
if (!writer_open_ && count_ == 0) {
    lock_.release();
    goto out;
}
```

当写端已关闭且缓冲区为空时返回 0（EOF 语义）。如果只有缓冲区空但写端还活着，就进入 spin-wait 等待新数据。

### 非阻塞读写 (pipe.cpp — try_read/try_write)

```cpp
int64_t Pipe::try_read(char* buf, uint64_t count) {
    if (buf == nullptr || count == 0) return -1;
    auto guard = lock_.guard();
    if (!writer_open_ && count_ == 0) return 0;
    if (count_ == 0) return 0;
    // ... same two-segment copy as read() ...
}
```

try_read 和 try_write 在缓冲区空/满时立即返回 0，不做任何等待。它们不需要 IRQ save/restore，因为不会被阻塞——获取锁后要么有数据直接操作，要么没数据直接返回。这个非阻塞版本是专门给 PIT 中断回调用的：在 tick 中断上下文中不能做阻塞操作。

### VFS 适配器 (pipe_ops.hpp/cpp)

```cpp
class PipeReadOps : public cinux::fs::InodeOps {
public:
    explicit PipeReadOps(Pipe* pipe);
    int64_t read(const cinux::fs::Inode* inode, uint64_t offset,
                 void* buf, uint64_t count) override;
private:
    Pipe* pipe_;
};

class PipeWriteOps : public cinux::fs::InodeOps {
public:
    explicit PipeWriteOps(Pipe* pipe);
    int64_t write(cinux::fs::Inode* inode, uint64_t offset,
                  const void* buf, uint64_t count) override;
private:
    Pipe* pipe_;
};
```

这两个适配器是对称的薄封装——PipeReadOps 把 read 请求委托给 Pipe::read，PipeWriteOps 把 write 请求委托给 Pipe::write。InodeOps 的 read/write 签名中有 offset 参数，但管道是不可寻址的字节流，这个参数被直接忽略。没有重写的方法（比如另一个方向的 read/write）走 InodeOps 的默认实现返回错误。

这个设计让管道无缝融入 VFS——对上层代码来说，管道 inode 和普通文件 inode 的使用方式完全一样，只是底层操作不同。

### sys_pipe 系统调用 (sys_pipe.cpp)

```cpp
int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    if (!is_user_addr(pipefd_virt)) return -1;

    auto* pipe = new cinux::ipc::Pipe();
    auto* read_ops  = new cinux::ipc::PipeReadOps(pipe);
    auto* write_ops = new cinux::ipc::PipeWriteOps(pipe);

    cinux::fs::Inode* read_inode = new cinux::fs::Inode();
    read_inode->ops  = read_ops;
    read_inode->type = cinux::fs::InodeType::Regular;

    cinux::fs::Inode* write_inode = new cinux::fs::Inode();
    write_inode->ops  = write_ops;
    write_inode->type = cinux::fs::InodeType::Regular;

    auto& table = cinux::fs::current_fd_table();
    int read_fd = table.alloc(read_inode, cinux::fs::OpenFlags::RDONLY);
    if (read_fd < 0) { /* cleanup */ return -1; }

    int write_fd = table.alloc(write_inode, cinux::fs::OpenFlags::WRONLY);
    if (write_fd < 0) { table.close(read_fd); /* cleanup */ return -1; }

    auto* pipefd = reinterpret_cast<int32_t*>(pipefd_virt);
    pipefd[0] = read_fd;
    pipefd[1] = write_fd;
    return 0;
}
```

sys_pipe 的流程很直观——创建 Pipe，创建两套 InodeOps + Inode，分配两个 fd，写入用户态。值得注意的是失败时的清理路径：如果 write_fd 分配失败，必须先 close 已分配的 read_fd，然后按逆序 delete 所有堆对象。地址验证函数 `is_user_addr` 检查 x86_64 canonical address 规则，拒绝内核空间地址。

## 设计决策

### 决策：环形缓冲区管理方式
**问题**: 用 head/tail/count 三变量还是 nread/nwrite 单调计数器？
**本项目的做法**: head/tail/count 三变量，count 直接表示缓冲区中的数据量。
**备选方案**: xv6 使用 nread/nwrite 单调递增计数器，取模计算索引。
**为什么不选备选方案**: 三变量方式更直观，count == 0 和 count == PIPE_BUFFER_SIZE 直接判断空/满，不需要额外的减法运算。在代码可读性方面更适合教学 OS。
**如果要扩展/改进**: 考虑改用无锁环形缓冲区（单生产者单消费者场景下可行），消除自旋锁开销。

### 决策：阻塞方式
**问题**: 缓冲区满/空时怎么等待？
**本项目的做法**: spin-wait + hlt()，有界迭代。
**备选方案**: 使用调度器的 sleep/wakeup 机制（需要条件变量）。
**为什么不选备选方案**: Cinux 当前的调度器还不支持在管道上阻塞（wait queue 机制尚未实现）。spin-wait 是一种临时方案，在单核场景下配合 IRQ 安全设计可以工作，但不是长久之计。
**如果要扩展/改进**: 实现 wait_queue，将当前进程移出就绪队列，由另一端的操作唤醒。这将大幅减少 CPU 浪费。

## 扩展方向

- **双向管道 (socketpair)**: 在一个 Pipe 基础上增加第二个反方向管道，实现双向通信 (难度: 中等)
- **有名管道 (named pipe/FIFO)**: 将管道注册到 VFS 中作为文件系统节点，允许无亲缘关系的进程通信 (难度: 较高)
- **管道容量动态调整**: 类似 Linux 的 fcntl(F_SETPIPE_SZ)，根据负载动态调整缓冲区大小 (难度: 中等)
- **无锁环形缓冲区**: 在 SPSC 场景下使用内存序保证替代自旋锁 (难度: 较高)
- **select/poll 机制**: 支持同时监控多个 fd 的可读/可写状态 (难度: 较高)

## 参考资料

- OSDev Wiki: [Unix Pipes](https://wiki.osdev.org/Unix_Pipes) — 管道的基本概念和实现指导
- xv6 RISC-V: [pipe.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c) — 经典教学 OS 的管道实现，使用 sleep/wakeup 阻塞
- Oracle Linux Blog: [Pipe and Splice](https://blogs.oracle.com/linux/pipe-and-splice) — Linux 内核多页管道缓冲区设计
