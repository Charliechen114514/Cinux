/**
 * @file kernel/syscall/sys_getcwd.cpp
 * @brief sys_getcwd handler implementation
 *
 * Copies the current task's cwd into the user-supplied buffer.
 */

#include "kernel/syscall/sys_getcwd.hpp"

#include <stdint.h>

#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, uint64_t,
                   uint64_t, uint64_t, uint64_t) {
    // Validate user pointer
    if (buf_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    if (size == 0) {
        return -1;
    }

    // Step 1: Get current task
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -1;
    }

    // Step 2: Compute cwd length (including NUL)
    uint32_t cwd_len = 0;
    while (current->cwd[cwd_len] != '\0') {
        ++cwd_len;
    }
    ++cwd_len;  // include NUL

    if (cwd_len > size) {
        return -1;
    }

    // Step 3: Copy cwd to user buffer
    auto* dst = reinterpret_cast<char*>(buf_virt);
    memcpy(dst, current->cwd, cwd_len);

    return static_cast<int64_t>(cwd_len);
}

}  // namespace cinux::syscall
