---
title: 028d-sync-safety-2 · 同步安全
---

# 028d-2 Read-through: PMM/Heap/Scheduler 加锁实现

## 概览

本文聚焦于 028d tag 的核心加锁实现——如何将上一章的 Spinlock 和 RAII 守卫应用到 PMM、Heap、Scheduler、VMM 以及文件系统组件中。我们将逐个文件讲解加锁策略和代码变更，重点关注"锁的拆分"（_locked 版本）和 IrqGuard 在调度器中的使用。

关键设计决策一览：PMM/Heap 采用公共方法加锁+内部 _locked 方法的拆分模式，Scheduler 使用 IrqGuard 保护运行队列操作，原子计数器用 `std::atomic` 替代普通变量，文件系统组件使用 Spinlock Guard 做轻量保护。

## 架构图

```
                TIER 0: 核心分配器
  ┌──────────┐  ┌──────────┐  ┌───────────────┐
  │ PMM      │  │ Heap     │  │ RoundRobin    │
  │ Guard    │  │ Guard    │  │ IrqGuard      │
  │ +locked  │  │ +locked  │  │               │
  └──────────┘  └──────────┘  └───────────────┘

                TIER 1: 原子计数器
  ┌──────────┐  ┌──────────┐  ┌───────────────┐
  │ PIT      │  │Scheduler │  │ TaskBuilder   │
  │ atomic   │  │ atomic   │  │ atomic        │
  │ tick_cnt │  │ tick/slc │  │ tid/stack     │
  └──────────┘  └──────────┘  └───────────────┘

                TIER 2: 中等风险组件
  ┌──────────┐  ┌──────────┐  ┌───────────────┐
  │ FDTable  │  │ File     │  │ VMM/mount     │
  │ Guard    │  │ offset   │  │ Guard         │
  │          │  │ Guard    │  │               │
  └──────────┘  └──────────┘  └───────────────┘
```

## 代码精讲

### PMM: 锁的拆分模式

PMM 的加锁是整个 tag 中最需要仔细设计的部分。核心思路是把原来的公共方法拆成"带锁的公共入口"和"不带锁的内部实现"。

先看不带锁的内部实现——它们直接操作位图，不获取任何锁：

```cpp
uint64_t PMM::alloc_page_locked() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;

    bm_set(bitmap_, static_cast<uint64_t>(idx));
    free_pages_--;
    return static_cast<uint64_t>(idx) * PAGE_SIZE;
}

void PMM::free_page_locked(uint64_t phys) {
    if (phys == 0) return;
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= highest_page_) return;
    if (!bm_test(bitmap_, idx)) return;

    bm_clear(bitmap_, idx);
    free_pages_++;
}
```

逻辑和之前的 `alloc_page`/`free_page` 完全一样——扫描位图找一个空闲位、设置为已用、减少空闲计数。唯一的区别是函数名后面加了 `_locked` 后缀，提醒调用者"你必须已经持有锁"。

然后是带锁的公共版本——它们只负责获取锁，然后委托给内部版本：

```cpp
uint64_t PMM::alloc_page() {
    auto g = lock_.guard();
    (void)g;
    return alloc_page_locked();
}

void PMM::free_page(uint64_t phys) {
    auto g = lock_.guard();
    (void)g;
    free_page_locked(phys);
}
```

这里的 `(void)g;` 看起来有点多余，但实际上是必要的。它告诉编译器"我确实在用这个变量"——防止编译器在 Release 模式下发出"未使用变量"警告。`g` 的实际作用是它的生命周期——从构造到析构，Spinlock 一直被持有。

更有趣的是 `alloc_pages`（分配连续多页）的实现。它也需要获取锁，但在持锁状态下需要调用单页分配的逻辑：

```cpp
uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 0) return 0;

    auto g = lock_.guard();
    (void)g;

    if (count == 1) return alloc_page_locked();
    // ... 多页分配的逻辑（直接操作位图，已在持锁状态）
```

注意 count==1 的分支调用的是 `alloc_page_locked()` 而不是 `alloc_page()`。如果错误地调用了 `alloc_page()`，后者会尝试再次获取 `lock_`——自旋锁不支持递归获取，同一个 CPU 两次 acquire 同一个自旋锁会永远自旋下去，也就是死锁。这个 `_locked` 拆分模式就是为了解决这个问题。

### Heap: alloc_locked 拆分

Heap 的加锁模式和 PMM 非常相似，但有一个额外的复杂度——alloc 在找不到合适 block 时会调用 expand 扩展堆，然后递归调用自身重试。

```cpp
void* Heap::alloc_locked(size_t size, size_t align) {
    // ... 空闲链表遍历和 first-fit 搜索 ...
    // ... 没找到合适的 block ...

    expand(size + align + HEADER_SIZE);
    return alloc_locked(size, align);  // 递归重试，走 _locked 版本
}
```

递归调用必须走 `alloc_locked` 而不是公共的 `alloc`——原因和 PMM 一样，公共版本会再次获取锁导致死锁。而 expand 内部调用 PMM 的 alloc_page 和 VMM 的 map 使用的是它们各自的公共版本（带锁的），这完全没问题——锁的对象不同（Heap 的 lock_ vs PMM 的 lock_ vs VMM 的 lock_），不会冲突。

公共的 `alloc` 只是一个薄薄的加锁包装：

```cpp
void* Heap::alloc(size_t size, size_t align) {
    auto g = lock_.guard();
    (void)g;
    return alloc_locked(size, align);
}
```

`free` 则不需要拆分——它没有递归调用的场景，直接在入口加锁即可：

```cpp
void Heap::free(void* ptr) {
    auto g = lock_.guard();
    (void)g;

    if (ptr == nullptr) return;
    // ... 释放逻辑 ...
}
```

### Scheduler: IrqGuard 保护运行队列

Scheduler 的 RoundRobin 调度器使用的不是普通 Guard，而是 IrqGuard。原因是 `tick()` 从时钟中断处理程序中被调用，中断处理程序可能在任何线程持锁时打断它。

```cpp
void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    if (count_ >= MAX_TASKS) {
        cinux::lib::kprintf("[SCHED] RoundRobin: run queue full\n");
        return;
    }
    // ... 入队逻辑 ...
}
```

`dequeue` 和 `pick_next` 完全相同的模式——入口处获取 `irq_guard()`。

Scheduler 的静态计数器则从普通 int 改为了 `std::atomic<int>`：

```cpp
// scheduler.hpp
static lib::Atomic<int> tick_count_;
static lib::Atomic<int> current_slice_;

// scheduler.cpp 中的操作
tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
current_slice_.fetch_add(1, lib::MemoryOrder::Relaxed);

if (current_slice_.load(lib::MemoryOrder::Relaxed) >= DEFAULT_TIME_SLICE) {
    current_slice_.store(0, lib::MemoryOrder::Relaxed);
    schedule();
}
```

这里全部使用 `MemoryOrder::Relaxed`——tick_count_ 和 current_slice_ 是独立的计数器，不需要与其他操作建立 happens-before 关系。我们只需要保证 fetch_add 本身是原子的（不会出现半写的状态），relaxed 就足够了。

### TaskBuilder: atomic TID 和栈地址

```cpp
// process.cpp
std::atomic<uint64_t> next_tid{1};
std::atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};

uint64_t alloc_stack_vaddr(uint64_t pages) {
    uint64_t vaddr = next_stack_vaddr.fetch_add(
        pages * cinux::arch::PAGE_SIZE, std::memory_order_relaxed);
    return vaddr;
}

// TaskBuilder::build() 中
task->tid = next_tid.fetch_add(1, std::memory_order_relaxed);
```

这里有一个很精妙的替换。原来的代码是"读取变量、使用旧值、加上偏移量"两步操作，改成 `fetch_add` 后变成一步原子操作——`fetch_add` 返回加之前的旧值，同时把新值写回。两个线程同时调用 `fetch_add` 绝不会得到相同的返回值——这正是 TID 分配和栈地址分配需要的保证。

### FDTable 和 File offset

FDTable 的三个操作都在入口处加锁：

```cpp
int FDTable::alloc(Inode* inode, OpenFlags flags) {
    auto g = lock_.guard();
    (void)g;
    for (uint32_t i = FD_FIRST; i < FD_TABLE_SIZE; ++i) {
        if (fds_[i] == nullptr) {
            fds_[i] = new File(inode, 0, flags);
            return static_cast<int>(i);
        }
    }
    return -1;
}
```

File 结构体新增了一个构造函数和 offset_lock_：

```cpp
struct File {
    File(Inode* in, uint64_t off, OpenFlags fl)
        : inode(in), offset(off), flags(fl) {}

    Inode*    inode;
    uint64_t  offset;
    OpenFlags flags;

    mutable cinux::proc::Spinlock offset_lock_;
};
```

`mutable` 关键字允许在 const 成员函数中修改 offset_lock_（因为获取和释放锁不算"修改 File 的逻辑状态"）。sys_read 中的使用方式：

```cpp
{
    auto g = file->offset_lock_.guard();
    (void)g;
    int64_t result = file->inode->ops->read(
        file->inode, file->offset, buf, count);
    if (result > 0) {
        file->offset += static_cast<uint64_t>(result);
    }
    return result;
}
```

用大括号创建一个作用域——guard 在这个作用域结束时析构，锁被释放。return 语句在作用域内，所以返回值在锁释放之前就已经拿到了。

### VMM 和 Mount 表

VMM 的 `map`/`unmap` 加锁模式完全一样——入口处获取 lock_.guard()。Mount 表用一个全局 Spinlock 保护三个函数：vfs_mount_add、vfs_mount_remove、vfs_resolve。

### Keyboard: InterruptGuard

键盘的 poll 使用 InterruptGuard 而非 Spinlock：

```cpp
bool Keyboard::poll(KeyEvent& out) {
    cinux::proc::InterruptGuard guard;
    (void)guard;

    if (head_ == tail_) return false;
    out = queue_[head_];
    head_ = (head_ + 1) % KEY_QUEUE_SIZE;
    return true;
}
```

IRQ1 handler 不获取锁——它在中断上下文中直接写入 ring buffer。poll 只需要确保读取时不会被 IRQ1 打断，InterruptGuard 完美满足这个需求。

### PIT: atomic tick_count

```cpp
// pit.hpp
static lib::Atomic<uint64_t> tick_count_;

// pit.cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
    PIC::send_eoi(0);
    Scheduler::tick();
}

uint64_t PIT::get_ticks() {
    return tick_count_.load(lib::MemoryOrder::Relaxed);
}
```

PIT 的 tick_count_ 是从 IRQ0 handler 中递增的，在各种地方被读取。用 `std::atomic` 的 `fetch_add` 和 `load` 替代普通的 `++` 和读取，开销极小（x86 上编译为 `lock inc` 或 `lock xadd`），但保证了原子性。

## 设计决策

### 决策：锁的拆分 vs 递归锁

**问题**: PMM 的 alloc_pages 需要在持锁状态下调用 alloc_page 的逻辑。是实现递归锁（同一线程可以多次获取），还是拆分出 _locked 内部版本？

**本项目的做法**: 拆分出 _locked 内部版本。

**备选方案**: 实现递归自旋锁（记录持有者，同一持有者可以多次 acquire）。

**为什么不选备选方案**: 递归锁增加了复杂度（需要记录持有者 Task 指针和一个递归计数器），而且掩盖了设计上的问题——通常递归锁的需求意味着 API 设计可以改进。拆分出 _locked 版本更清晰地表达了"这个函数假设锁已被持有"的契约。Linux 内核也不使用递归自旋锁（`mutex` 支持递归但 `spinlock` 不支持）。

### 决策：IrqGuard vs Guard 的选择标准

**问题**: 什么时候用 IrqGuard，什么时候用普通 Guard？

**本项目的做法**: 如果被保护的操作可能在中断处理程序中被调用（Scheduler 的 enqueue/dequeue/pick_next），用 IrqGuard。否则用普通 Guard。

**如果要扩展/改进**: 可以引入 `preempt_disable`/`preempt_enable` 计数器，在 spinlock 持有期间自动禁用抢占——这比 IrqGuard 更轻量（只禁止调度，不禁止中断），适合那些不需要中断安全但需要调度安全的场景。

## 扩展方向

- 为 PMM 的 free_pages 实现 batched free——先在局部收集要释放的页面列表，一次性获取锁批量释放
- 考虑 VMM 的 map 操作是否可以在持锁期间避免调用 PMM alloc（用 lock-free 的页面预留机制）
- 实现 RCU（Read-Copy-Update）模式来保护 mount 表的读操作——读端不需要锁
- 为 FDTable 实现 per-fd 细粒度锁，替代整个表的单一锁
- 研究读写锁（RW-Lock）的实现，分析哪些组件（如 mount 表）更适合读写锁

## 参考资料

- Intel SDM: Vol.3A Section 9.1 — 原子操作与 LOCK 前缀
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives)
- Linux: `kernel/locking/spinlock.c` — `spinlock_irqsave` 实现
- xv6-riscv: `kernel/spinlock.c` — `push_off`/`pop_off` 中断嵌套
