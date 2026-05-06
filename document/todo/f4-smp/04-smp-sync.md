# M5: 多核同步原语

> 将 Spinlock 升级为 ticket lock（公平性）。
> 新增 Per-CPU 变量机制和扩展原子操作库。

## 依赖

- M3 Per-CPU 数据机制

## 任务清单

### T1: Ticket Lock

**文件**: `kernel/proc/sync.hpp`（改造）

替换当前 test-and-set Spinlock 为 ticket lock：

```cpp
class TicketLock {
public:
    void lock() {
        uint16_t ticket = next_ticket_.fetch_add(1, MemoryOrder::Relaxed);
        while (serving_.load(MemoryOrder::Acquire) != ticket) {
            __asm__ volatile("pause");
        }
    }

    void unlock() {
        serving_.fetch_add(1, MemoryOrder::Release);
    }

    bool try_lock();

private:
    Atomic<uint16_t> next_ticket_{0};
    Atomic<uint16_t> serving_{0};
};
```

- [ ] TicketLock 实现
- [ ] 替换现有 Spinlock 的内部实现（保持外部接口不变）
- [ ] guard() / irq_guard() RAII 包装不变

### T2: Per-CPU 变量宏

**文件**: `kernel/arch/x86_64/percpu.hpp`

提供 Linux 风格的 Per-CPU 变量声明和访问：

```cpp
// 声明 Per-CPU 变量（在 PerCpu 结构中添加字段）
#define DEFINE_PER_CPU(type, name) \
    type name __attribute__((section(".percpu")))

// 访问当前 CPU 的变量
#define this_cpu(name) (percpu()->name)
```

- [ ] PerCpu 结构扩展宏
- [ ] this_cpu() 快速访问
- [ ] 禁止抢占保护（Per-CPU 访问期间）

### T3: 原子操作库扩展

**文件**: `kernel/lib/atomic.hpp`（增强）

```cpp
template <typename T>
class Atomic {
public:
    T load(MemoryOrder order = MemoryOrder::Relaxed);
    void store(T val, MemoryOrder order = MemoryOrder::Relaxed);
    T fetch_add(T val, MemoryOrder order);
    T fetch_sub(T val, MemoryOrder order);
    bool compare_exchange(T& expected, T desired, MemoryOrder order);
    T exchange(T desired, MemoryOrder order);

    // 位操作
    T fetch_or(T val, MemoryOrder order);
    T fetch_and(T val, MemoryOrder order);
};

enum class MemoryOrder {
    Relaxed = __ATOMIC_RELAXED,
    Acquire = __ATOMIC_ACQUIRE,
    Release = __ATOMIC_RELEASE,
    AcqRel  = __ATOMIC_ACQ_REL,
    SeqCst  = __ATOMIC_SEQ_CST,
};
```

- [ ] 使用 `__atomic_*` GCC 内建函数实现
- [ ] 支持所有标准 memory order
- [ ] 特化 uint8_t / uint16_t / uint32_t / uint64_t

### T4: 读写锁（可选）

```cpp
class RWLock {
public:
    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();

private:
    Atomic<uint32_t> readers_{0};
    Atomic<bool>     writer_{false};
};
```

- [ ] 简单读写锁（读者并发，写者独占）
- [ ] 适合 VFS dentry cache 等读多写少场景

### T5: 单元测试

- [ ] TicketLock 公平性（先到先得）
- [ ] Atomic 操作正确性（多核并发）
- [ ] Per-CPU 变量隔离
- [ ] RWLock 读写互斥

## 产出物

- [ ] `kernel/proc/sync.hpp` — TicketLock 替换
- [ ] `kernel/lib/atomic.hpp` — 完整 Atomic<T>
- [ ] Per-CPU 变量机制
- [ ] RWLock（可选）
- [ ] 向后兼容（现有 Spinlock 使用方不需要改动）
