/**
 * @file kernel/syscall/sys_write.cpp
 * @brief sys_write handler implementation
 *
 * Validates the user buffer address and outputs to kprintf (serial + Console).
 */

#include "kernel/syscall/sys_write.hpp"

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;

}  // anonymous namespace

int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) {
        return -1;
    }

    if (fd != 1) {
        return -1;
    }

    const auto* buf = reinterpret_cast<const char*>(buf_virt);
    for (uint64_t i = 0; i < count; i++) {
        kprintf("%c", buf[i]);
    }

    return static_cast<int64_t>(count);
}

}  // namespace cinux::syscall
