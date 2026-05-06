# M1: Pipe 等待队列增强

> 将当前 pipe 的轮询等待改为 Mutex + 等待队列阻塞。
> 复用 F1 的 RingBuffer 库。

## 任务清单

### T1: 替换 Ring Buffer

**文件**: `kernel/ipc/pipe.hpp`

当前手动管理的 head/tail/count → 改用 F1 的 `ByteRingBuffer<4096>`：

- [ ] 内部 buffer_ 改为 ByteRingBuffer<4096>
- [ ] read/write 使用 ByteRingBuffer 的 push/pop
- [ ] 移除手动 index 管理

### T2: 等待队列阻塞

替代当前 1,000,000 次 hlt() 轮询：

```cpp
class Pipe {
    Mutex lock_;
    ConditionVariable read_cv_;   // 缓冲区非空条件
    ConditionVariable write_cv_;  // 缓冲区非满条件
};
```

- [ ] read：缓冲区空 → wait(read_cv_)，被写入时 signal
- [ ] write：缓冲区满 → wait(write_cv_)，被读取时 signal
- [ ] close_reader / close_writer → 广播唤醒等待者
- [ ] 非阻塞模式支持 (O_NONBLOCK)

### T3: ConditionVariable 实现

**文件**: `kernel/proc/sync.hpp`

```cpp
class ConditionVariable {
public:
    void wait(Mutex& mutex);
    bool wait_for(Mutex& mutex, uint64_t timeout_ms);
    void signal();     // 唤醒一个
    void broadcast();  // 唤醒所有
private:
    Task* waiters_;    // 等待队列
};
```

- [ ] wait：释放 mutex + block + 重新获取 mutex
- [ ] signal：unblock 队头
- [ ] broadcast：unblock 全部

## 产出物

- [ ] `kernel/ipc/pipe.hpp` — 改用 RingBuffer + 等待队列
- [ ] `kernel/proc/sync.hpp` — ConditionVariable
