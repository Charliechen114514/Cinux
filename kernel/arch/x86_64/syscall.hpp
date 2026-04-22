/**
 * @file kernel/arch/x86_64/syscall.hpp
 * @brief SYSCALL/SYSRET based system call infrastructure
 *
 * Provides the kernel-side system call framework:
 *   - syscall_init() configures LSTAR, STAR, and SFMASK MSRs so that
 *     the SYSCALL instruction from Ring 3 enters syscall_entry in Ring 0.
 *   - syscall_dispatch() is called from syscall.S with the syscall number
 *     in %rax and up to six arguments in the System V AMD64 ABI registers.
 *   - Individual handlers are registered in a dispatch table.
 *
 * MSR layout (Intel SDM Vol. 2A, SYSCALL/SYSRET):
 *   STAR    (0xC0000081): [63:48] = SYSRET CS base, [47:32] = SYSCALL CS base
 *   LSTAR   (0xC0000082): SYSCALL entry point (RIP loaded on SYSCALL)
 *   SFMASK  (0xC0000084): RFLAGS bits cleared on SYSCALL entry
 *
 * Dependencies:
 *   - GDT must be initialised (kernel/user segment selectors)
 *   - EFER.SCE must be enabled (done in usermode_init)
 *   - Scheduler must be initialised for sys_exit / sys_yield
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>
#include "kernel/syscall/syscall_nums.hpp"

namespace cinux::arch {

using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);

/**
 * @brief Initialise the SYSCALL entry point and dispatch table
 *
 * Writes LSTAR MSR with the address of syscall_entry, configures
 * SFMASK to clear IF on entry, and sets up the STAR MSR with
 * kernel/user segment selectors.  Also initialises the per-CPU
 * kernel stack pointer used by syscall_entry.
 *
 * Must be called once during boot, after GDT and usermode_init().
 *
 * @param kernel_rsp  The kernel stack pointer to use on SYSCALL entry
 */
void syscall_init(uint64_t kernel_rsp);

/**
 * @brief Register a syscall handler in the dispatch table
 *
 * @param nr      Syscall number
 * @param handler Function pointer implementing the syscall
 */
void syscall_register(cinux::syscall::SyscallNr nr, SyscallFn handler);

/**
 * @brief Get the per-CPU kernel RSP used by syscall_entry
 *
 * @return The saved kernel stack pointer
 */
uint64_t syscall_get_kernel_rsp();

}  // namespace cinux::arch

// ============================================================
// Assembly entry points (C linkage)
// ============================================================

extern "C" {

/**
 * @brief SYSCALL entry point (called from Ring 3 via SYSCALL instruction)
 *
 * This function is the target of the LSTAR MSR.  It must:
 *   1. SWAPGS (swap GS base to kernel's GS)
 *   2. Switch to the per-CPU kernel stack
 *   3. Save user RSP, RCX, R11, and argument registers
 *   4. Call syscall_dispatch
 *   5. Restore state and return via SYSRETQ
 *
 * Never call this directly from C code.
 */
void syscall_entry();

/**
 * @brief C-level syscall dispatcher called from syscall_entry
 *
 * @param nr    Syscall number (from RAX at SYSCALL time)
 * @param a1    First argument (RDI)
 * @param a2    Second argument (RSI)
 * @param a3    Third argument (RDX)
 * @param a4    Fourth argument (R10)
 * @param a5    Fifth argument (R8)
 * @param a6    Sixth argument (R9)
 * @return Return value for the user program (placed in RAX)
 */
int64_t syscall_dispatch(uint64_t nr,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);

}  // extern "C"
