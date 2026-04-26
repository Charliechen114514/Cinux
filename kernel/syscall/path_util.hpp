/**
 * @file kernel/syscall/path_util.hpp
 * @brief Shared path resolution helpers for path-based syscalls
 *
 * Provides validate_user_string() and resolve_user_path() so that
 * individual syscalls do not duplicate address validation and
 * cwd-aware path resolution logic.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/path.hpp"

namespace cinux::syscall {

/**
 * @brief Validate a user-space pointer (canonical address check)
 *
 * @return true if the address looks like a valid user-space pointer
 */
inline bool validate_user_ptr(uint64_t ptr) {
    if (ptr == 0) {
        return false;
    }
    uint64_t bit47 = (ptr >> 47) & 1;
    uint64_t upper = ptr >> 48;
    if (bit47 == 0 && upper != 0) {
        return false;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return false;
    }
    return true;
}

/**
 * @brief Resolve a user-space path to an absolute kernel-side path
 *
 * Validates the user pointer, then resolves the path relative to
 * the current task's cwd.  The result is canonicalised.
 *
 * @param path_virt  User virtual address of the path string
 * @param out        Output buffer (at least cinux::fs::PATH_MAX bytes)
 * @return true on success, false on error
 */
bool resolve_user_path(uint64_t path_virt, char* out);

}  // namespace cinux::syscall
