/**
 * @file kernel/syscall/path_util.cpp
 * @brief Shared path resolution helper implementation
 */

#include "kernel/syscall/path_util.hpp"

#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

bool resolve_user_path(uint64_t path_virt, char* out) {
    if (!validate_user_ptr(path_virt)) {
        return false;
    }

    auto* path = reinterpret_cast<const char*>(path_virt);

    if (path[0] == '\0') {
        return false;
    }

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char* cwd = (current != nullptr) ? current->cwd : "/";

    return cinux::fs::path_resolve(cwd, path, out);
}

}  // namespace cinux::syscall
