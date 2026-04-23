#pragma once

#include <stdint.h>
#include <stddef.h>

#include "kernel/proc/process.hpp"

namespace cinux::proc {

class SchedulingClass {
public:
    virtual ~SchedulingClass() = default;

    virtual void enqueue(Task* task) = 0;
    virtual void dequeue(Task* task) = 0;
    virtual Task* pick_next() = 0;
    virtual const char* name() const = 0;
};

class RoundRobin : public SchedulingClass {
public:
    static constexpr int MAX_TASKS = 64;

    RoundRobin();

    void enqueue(Task* task) override;
    void dequeue(Task* task) override;
    Task* pick_next() override;
    const char* name() const override;

private:
    Task* run_queue_[MAX_TASKS];
    int head_;
    int tail_;
    int count_;
};

class Scheduler {
public:
    static constexpr int MAX_CLASSES = 4;
    static constexpr int DEFAULT_TIME_SLICE = 2;

    static void init();
    static void register_class(SchedulingClass* sched_class);
    static void add_task(Task* task);
    static void remove_task(Task* task);
    static void yield();
    static void exit_current();
    static void run_first(Task* boot_task);
    static Task* current();
    static void set_current(Task* task);
    static bool is_initialized();

    static void tick();
    static void schedule();
    static void block(Task* task, const char* reason);
    static void unblock(Task* task);

private:
    static void idle_entry();

    static SchedulingClass* classes_[MAX_CLASSES];
    static int class_count_;
    static Task* current_;
    static RoundRobin default_rr_;
    static Task* idle_task_;
    static bool initialized_;
    static int tick_count_;
    static int current_slice_;
};

}  // namespace cinux::proc
