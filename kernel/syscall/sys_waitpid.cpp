/**
 * @file kernel/syscall/sys_waitpid.cpp
 * @brief sys_waitpid handler implementation
 *
 * Delegates to cinux::proc::waitpid() which searches the caller's
 * children list for a zombie child, collects the exit status, and
 * reaps the child TCB.
 */

#include "kernel/syscall/sys_waitpid.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::syscall {

int64_t sys_waitpid(uint64_t pid_arg, uint64_t status_arg, uint64_t, uint64_t, uint64_t, uint64_t) {
    int  pid    = static_cast<int>(pid_arg);
    int* status = reinterpret_cast<int*>(status_arg);

    auto result = cinux::proc::waitpid(pid, status, cinux::proc::g_pid_alloc);

    if (result == cinux::proc::WaitpidResult::Ok) {
        // Return the reaped child's PID
        return static_cast<int64_t>(pid);
    }

    if (result == cinux::proc::WaitpidResult::NotExited) {
        // Child exists but has not exited yet; return 0 (non-blocking)
        return 0;
    }

    // Error: return negated errno
    return static_cast<int64_t>(result);
}

}  // namespace cinux::syscall
