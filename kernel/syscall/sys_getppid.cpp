/**
 * @file kernel/syscall/sys_getppid.cpp
 * @brief sys_getppid handler implementation
 *
 * Returns the parent PID of the current task by reading from the TCB.
 */

#include "kernel/syscall/sys_getppid.hpp"

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_getppid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -1;
    }
    return static_cast<int64_t>(task->ppid);
}

}  // namespace cinux::syscall
