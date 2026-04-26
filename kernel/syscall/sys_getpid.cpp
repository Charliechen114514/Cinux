/**
 * @file kernel/syscall/sys_getpid.cpp
 * @brief sys_getpid handler implementation
 *
 * Returns the PID of the current task by reading from the TCB.
 */

#include "kernel/syscall/sys_getpid.hpp"

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -1;
    }
    return static_cast<int64_t>(task->pid);
}

}  // namespace cinux::syscall
