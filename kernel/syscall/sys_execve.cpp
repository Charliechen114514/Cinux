/**
 * @file kernel/syscall/sys_execve.cpp
 * @brief sys_execve handler implementation
 *
 * Delegates to cinux::proc::execve() which performs the actual
 * ELF loading and address space setup.
 *
 * For now, the path argument is passed as a kernel pointer directly.
 * In a future milestone, user-space pointer validation and copying
 * will be added.
 */

#include "kernel/syscall/sys_execve.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::syscall {

int64_t sys_execve(uint64_t path_virt, uint64_t argv_virt, uint64_t envp_virt, uint64_t, uint64_t,
                   uint64_t) {
    // For this milestone, path_virt is treated as a kernel pointer.
    // Future: validate and copy from user-space.
    const char*        path = reinterpret_cast<const char*>(path_virt);
    const char* const* argv = reinterpret_cast<const char* const*>(argv_virt);
    const char* const* envp = reinterpret_cast<const char* const*>(envp_virt);

    auto result = cinux::proc::execve(path, argv, envp);

    if (result != cinux::proc::ExecveResult::Ok) {
        cinux::lib::kprintf("[SYSCALL] execve failed: %d\n", static_cast<int>(result));
        return static_cast<int64_t>(result);
    }

    return 0;
}

}  // namespace cinux::syscall
