#include "kernel/proc/scheduler.hpp"

#include "kernel/lib/kprintf.hpp"

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
// Scheduler static state
// ============================================================

SchedulingClass* Scheduler::classes_[Scheduler::MAX_CLASSES];
int Scheduler::class_count_ = 0;
Task* Scheduler::current_ = nullptr;
RoundRobin Scheduler::default_rr_;

// ============================================================
// Scheduler implementation
// ============================================================

void Scheduler::init() {
    class_count_ = 0;
    current_ = nullptr;
    register_class(&default_rr_);
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

void Scheduler::yield() {
    if (current_ == nullptr) {
        return;
    }

    SchedulingClass* cls = current_->sched_class;
    if (cls == nullptr) {
        return;
    }

    Task* next = cls->pick_next();
    if (next == nullptr || next == current_) {
        return;
    }

    Task* prev = current_;
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
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
        cinux::lib::kprintf("[SCHED] No more tasks, halting.\n");
        while (1) __asm__ volatile("cli; hlt");
    }

    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
}

void Scheduler::run_first(Task* boot_task) {
    current_ = boot_task;

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        return;
    }

    current_ = next;
    context_switch(&boot_task->ctx, &next->ctx);
}

Task* Scheduler::current() {
    return current_;
}

}  // namespace cinux::proc
