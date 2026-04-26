# 同步原语

> 里程碑: `021_proc_sync` `028d_sync_safety`

## 功能概述

内核同步原语和全局并发安全审计。提供 Spinlock、Mutex、Semaphore，以及 RAII guard 模式和中断保护。

## Spinlock (`kernel/proc/sync.hpp`)
- `acquire()` — `__atomic_test_and_set` + `pause` 自旋
- `release()` — `__atomic_clear`
- `[[nodiscard]] guard()` — RAII 自动释放

## Mutex (`kernel/proc/sync.hpp`)
- `{spin_, *owner_, *wait_head_}` — 内部用 Spinlock + 等待队列
- `lock()` — 已锁则 `block` 当前 task
- `unlock()` — 唤醒等待队列头
- `try_lock()` — 非阻塞尝试
- `[[nodiscard]] guard()` — RAII

## Semaphore (`kernel/proc/sync.hpp`)
- `{spin_, count_, *wait_head_}`
- `post()` — V 操作 (count++, 唤醒)
- `wait()` — P 操作 (count--, 负数则阻塞)
- `try_wait()` — 非阻塞

## 辅助 RAII
- `InterruptGuard` — pushfq/cli/popfq 自动恢复中断状态
- `Spinlock::IrqGuard` — 关中断 + 自旋锁组合 RAII

## 并发安全审计 (028d)

### TIER 0 — 核心分配器 (数据竞争必然崩溃)
| 组件 | 保护方式 |
|------|----------|
| PMM | Spinlock: alloc/free |
| Heap | Spinlock: alloc/free |
| Scheduler | Spinlock: enqueue/dequeue/pick_next |
| FDTable | Spinlock: alloc/close/get |

### TIER 1 — 高频共享计数器
| 组件 | 保护方式 |
|------|----------|
| PIT::tick_count_ | 原子操作 |
| Scheduler::tick_count_ | 原子操作 |
| TaskBuilder::next_tid | 原子操作 |
| Keyboard ring buffer | 关中断保护 |

### TIER 2 — 中等风险组件
| 组件 | 保护方式 |
|------|----------|
| File::offset | Spinlock |
| VMM::map/unmap | Spinlock |
| g_mount_table | Spinlock |

## 源码位置
- `kernel/proc/sync.hpp/cpp`
- `kernel/lib/atomic.hpp`
