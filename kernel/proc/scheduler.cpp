#include "kernel/proc/scheduler.hpp"

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/per_cpu.hpp"

namespace cinux::proc {

// ============================================================
// RoundRobin implementation
// ============================================================

RoundRobin::RoundRobin()
    : head_(0), tail_(0), count_(0) {
    for (int i = 0; i < MAX_TASKS; i++) {
        run_queue_[i] = nullptr;
    }
}

void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    if (count_ >= MAX_TASKS) {
        cinux::lib::kprintf("[SCHED] RoundRobin: run queue full\n");
        return;
    }
    run_queue_[tail_] = task;
    tail_ = (tail_ + 1) % MAX_TASKS;
    count_++;
    task->state = TaskState::Ready;
}

void RoundRobin::dequeue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    for (int i = 0; i < count_; i++) {
        int idx = (head_ + i) % MAX_TASKS;
        if (run_queue_[idx] == task) {
            for (int j = i; j < count_ - 1; j++) {
                int cur = (head_ + j) % MAX_TASKS;
                int nxt = (head_ + j + 1) % MAX_TASKS;
                run_queue_[cur] = run_queue_[nxt];
            }
            run_queue_[(head_ + count_ - 1) % MAX_TASKS] = nullptr;
            tail_ = (tail_ - 1 + MAX_TASKS) % MAX_TASKS;
            count_--;
            return;
        }
    }
}

Task* RoundRobin::pick_next() {
    auto g = lock_.irq_guard();
    (void)g;
    if (count_ == 0) {
        return nullptr;
    }
    Task* task = run_queue_[head_];
    head_ = (head_ + 1) % MAX_TASKS;
    count_--;

    task->state = TaskState::Running;

    run_queue_[tail_] = task;
    tail_ = (tail_ + 1) % MAX_TASKS;
    count_++;

    return task;
}

const char* RoundRobin::name() const {
    return "RoundRobin";
}

// ============================================================
// PerCPU global
// ============================================================

PerCPU g_per_cpu{nullptr, 0};

// ============================================================
// Scheduler static state
// ============================================================

SchedulingClass* Scheduler::classes_[Scheduler::MAX_CLASSES];
int Scheduler::class_count_ = 0;
Task* Scheduler::current_ = nullptr;
RoundRobin Scheduler::default_rr_;
Task* Scheduler::idle_task_ = nullptr;
bool Scheduler::initialized_ = false;
std::atomic<int> Scheduler::tick_count_{0};
std::atomic<int> Scheduler::current_slice_{0};

// ============================================================
// Scheduler implementation
// ============================================================

void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");
    }
}

void Scheduler::init() {
    class_count_ = 0;
    current_ = nullptr;
    idle_task_ = nullptr;
    tick_count_.store(0, std::memory_order_relaxed);
    current_slice_.store(0, std::memory_order_relaxed);
    register_class(&default_rr_);

    idle_task_ = TaskBuilder()
        .set_entry(idle_entry)
        .set_name("idle")
        .set_priority(255)
        .build();

    if (idle_task_ != nullptr) {
        idle_task_->state = TaskState::Ready;
        cinux::lib::kprintf("[SCHED] Idle task created tid=%u\n", idle_task_->tid);
    }

    initialized_ = true;
    cinux::lib::kprintf("[SCHED] Scheduler initialised with %s class\n",
                        default_rr_.name());
}

void Scheduler::register_class(SchedulingClass* sched_class) {
    if (class_count_ >= MAX_CLASSES) {
        cinux::lib::kprintf("[SCHED] Too many scheduling classes\n");
        return;
    }
    classes_[class_count_++] = sched_class;
}

void Scheduler::add_task(Task* task) {
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);
    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' added to %s\n",
                        task->tid, task->name, task->sched_class->name());
}

void Scheduler::remove_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }
    task->state = TaskState::Dead;
    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' removed\n",
                        task->tid, task->name);
}

void Scheduler::yield() {
    if (current_ == nullptr) {
        return;
    }

    schedule();
}

void Scheduler::exit_current() {
    Task* prev = current_;
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
        cinux::lib::kprintf("[SCHED] Task tid=%u '%s' exited\n",
                            prev->tid, prev->name);
    }

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        if (idle_task_ != nullptr) {
            next = idle_task_;
        } else {
            cinux::lib::kprintf("[SCHED] No more tasks, halting.\n");
            while (1) __asm__ volatile("cli; hlt");
        }
    }

    current_ = next;
    g_per_cpu.current = next;
    if (next != idle_task_) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    }
    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

void Scheduler::run_first(Task* boot_task) {
    current_ = boot_task;
    g_per_cpu.current = boot_task;
    cinux::arch::GDT::tss_set_rsp0(boot_task->kernel_stack_top);
    current_slice_.store(0, std::memory_order_relaxed);

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        return;
    }

    current_ = next;
    g_per_cpu.current = next;
    cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    __asm__ volatile("fxsave %0" : : "m"(boot_task->fpu_state));
    context_switch(&boot_task->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

Task* Scheduler::current() {
    return current_;
}

void Scheduler::set_current(Task* task) {
    current_ = task;
    g_per_cpu.current = task;
}

bool Scheduler::is_initialized() {
    return initialized_;
}

void Scheduler::tick() {
    if (!initialized_ || current_ == nullptr) {
        return;
    }

    tick_count_.fetch_add(1, std::memory_order_relaxed);
    current_slice_.fetch_add(1, std::memory_order_relaxed);

    if (current_slice_.load(std::memory_order_relaxed) >= DEFAULT_TIME_SLICE) {
        current_slice_.store(0, std::memory_order_relaxed);
        schedule();
    }
}

void Scheduler::schedule() {
    if (current_ == nullptr) {
        return;
    }

    Task* prev = current_;

    if (prev->state == TaskState::Running) {
        prev->state = TaskState::Ready;
    }

    Task* next = default_rr_.pick_next();

    if (next == nullptr || next == prev) {
        if (prev->state != TaskState::Blocked && prev->state != TaskState::Dead) {
            prev->state = TaskState::Running;
            return;
        }

        if (idle_task_ != nullptr && idle_task_ != prev) {
            next = idle_task_;
        } else {
            return;
        }
    }

    current_ = next;
    g_per_cpu.current = next;
    current_slice_.store(0, std::memory_order_relaxed);

    if (next != idle_task_) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    }

    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

void Scheduler::block(Task* task, const char* reason) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Blocked;
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' blocked: %s\n",
                        task->tid, task->name, reason ? reason : "unknown");

    if (task == current_) {
        schedule();
    }
}

void Scheduler::unblock(Task* task) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Ready;
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' unblocked\n",
                        task->tid, task->name);
}

}  // namespace cinux::proc
