/**
 * @file kernel/arch/x86_64/syscall.cpp
 * @brief SYSCALL/SYSRET based system call infrastructure implementation
 *
 * Implements syscall_init() which configures the SYSCALL MSRs,
 * syscall_dispatch() which routes system calls to the appropriate handler,
 * and syscall_register() for handler registration.
 */

#include "kernel/arch/x86_64/syscall.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_exit.hpp"
#include "kernel/syscall/sys_yield.hpp"
#include "kernel/syscall/sys_getdents.hpp"

namespace cinux::arch {

extern "C" void syscall_entry();

// ============================================================
// Internal state
// ============================================================

namespace {

using cinux::lib::kprintf;

SyscallFn syscall_table[cinux::syscall::SYSCALL_TABLE_SIZE] = {};

uint64_t g_syscall_kernel_rsp = 0;

void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile(
        "wrmsr"
        :
        : "c"(msr),
          "a"(static_cast<uint32_t>(value & 0xFFFFFFFF)),
          "d"(static_cast<uint32_t>(value >> 32))
    );
}

/// Register all built-in syscall handlers into the dispatch table
void register_builtin_handlers() {
    using namespace cinux::syscall;
    syscall_register(SyscallNr::SYS_read,  sys_read);
    syscall_register(SyscallNr::SYS_write, sys_write);
    syscall_register(SyscallNr::SYS_open,  sys_open);
    syscall_register(SyscallNr::SYS_close, sys_close);
    syscall_register(SyscallNr::SYS_exit,  sys_exit);
    syscall_register(SyscallNr::SYS_yield, sys_yield);
    syscall_register(SyscallNr::SYS_getdents, sys_getdents);
}

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

void syscall_init() {
    // Capture the current kernel stack pointer for syscall_entry
    uint64_t kernel_rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp));
    g_syscall_kernel_rsp = kernel_rsp;

    for (uint64_t i = 0; i < cinux::syscall::SYSCALL_TABLE_SIZE; i++) {
        syscall_table[i] = nullptr;
    }

    constexpr uint32_t MSR_STAR   = 0xC0000081;
    constexpr uint32_t MSR_LSTAR  = 0xC0000082;
    constexpr uint32_t MSR_SFMASK = 0xC0000084;

    uint64_t star_val = (static_cast<uint64_t>(GDT_SYSRET_BASE) << 48)
                      | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32);
    write_msr(MSR_STAR, star_val);

    uint64_t entry_addr = reinterpret_cast<uint64_t>(syscall_entry);
    write_msr(MSR_LSTAR, entry_addr);

    uint64_t sfmask_val = 0x200;
    write_msr(MSR_SFMASK, sfmask_val);

    // Register all built-in syscall handlers
    register_builtin_handlers();

    kprintf("[SYSCALL] LSTAR=%p STAR configured SFMASK=0x200 (clear IF)\n",
            reinterpret_cast<void*>(entry_addr));
}

void syscall_register(cinux::syscall::SyscallNr nr, SyscallFn handler) {
    uint64_t idx = static_cast<uint64_t>(nr);
    if (idx >= cinux::syscall::SYSCALL_TABLE_SIZE) {
        return;
    }
    syscall_table[idx] = handler;
}

uint64_t syscall_get_kernel_rsp() {
    return g_syscall_kernel_rsp;
}

}  // namespace cinux::arch

// ============================================================
// C-linkage dispatcher (called from syscall.S)
// ============================================================

extern "C" int64_t syscall_dispatch(uint64_t nr,
                                    uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    if (nr >= cinux::syscall::SYSCALL_TABLE_SIZE) {
        cinux::lib::kprintf("[DISPATCH] invalid syscall nr=%u\n",
                            static_cast<unsigned>(nr));
        return -1;
    }

    auto fn = cinux::arch::syscall_table[nr];
    if (fn == nullptr) {
        cinux::lib::kprintf("[SYSCALL] unhandled syscall %u\n",
                            static_cast<unsigned>(nr));
        return -1;
    }

    return fn(a1, a2, a3, a4, a5, a6);
}
