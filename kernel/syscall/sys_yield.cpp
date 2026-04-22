/**
 * @file kernel/syscall/sys_yield.cpp
 * @brief sys_yield handler implementation
 *
 * Delegates directly to the scheduler yield.
 */

#include "kernel/syscall/sys_yield.hpp"

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_yield(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    cinux::proc::Scheduler::yield();
    return 0;
}

}  // namespace cinux::syscall
