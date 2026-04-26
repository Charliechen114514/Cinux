/**
 * @file kernel/syscall/sys_exit.cpp
 * @brief sys_exit handler implementation
 *
 * Marks the current task as Dead and yields to the scheduler.
 */

#include "kernel/syscall/sys_exit.hpp"

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        task->state = cinux::proc::TaskState::Dead;
        cinux::lib::kprintf("[SYSCALL] sys_exit(%u) from tid=%u '%s'\n",
                            static_cast<unsigned>(code), static_cast<unsigned>(task->tid),
                            task->name);
    }

    if (cinux::proc::Scheduler::is_initialized()) {
        cinux::proc::Scheduler::yield();
    } else {
        cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }

    return 0;
}

}  // namespace cinux::syscall
