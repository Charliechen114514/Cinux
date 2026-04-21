/**
 * @file kernel/arch/x86_64/usermode.hpp
 * @brief User-mode (Ring 3) transition support
 *
 * Provides functions to initialise the SYSRET/SYSCALL infrastructure,
 * set up a minimal user address space, and jump from kernel mode (Ring 0)
 * to user mode (Ring 3).
 *
 * usermode_init() configures the STAR and EFER MSRs so that SYSRET
 * can transition to Ring 3.  launch_first_user() creates a minimal
 * user program page and user stack, then performs the SYSRET jump.
 *
 * Dependencies:
 *   - GDT must be initialised (user code/data selectors at 0x1B/0x23)
 *   - TSS must be loaded (RSP0 used on privilege level switches)
 *   - IDT must be set up (exceptions like #GP handled in Ring 0)
 *   - PMM must be initialised (for user page allocation)
 *   - AddressSpace must be initialised (for per-process page tables)
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::mm {
class AddressSpace;
}

namespace cinux::arch {

// ============================================================
// User-mode constants
// ============================================================

/// Default virtual address for user program entry (linker base)
constexpr uint64_t USER_ENTRY_BASE = 0x400000;

/// Default virtual address for the top of the user stack
constexpr uint64_t USER_STACK_TOP = 0x7FFFFF000;

/// Number of 4 KB pages for the user stack (16 KB)
constexpr uint64_t USER_STACK_PAGES = 4;

// ============================================================
// User-mode functions
// ============================================================

/**
 * @brief Initialise the STAR / EFER MSRs for SYSRET transitions
 *
 * Must be called once during boot, after GDT and IDT are initialised.
 * Configures the STAR MSR with kernel CS = 0x08 so that SYSRET
 * computes user CS = 0x1B (Ring 3 code) and user SS = 0x23 (Ring 3 data).
 */
void usermode_init();

/**
 * @brief Set up user pages and launch the first user-mode program
 *
 * Creates an AddressSpace with:
 *   - One code page at USER_ENTRY_BASE containing a CLI instruction
 *   - USER_STACK_PAGES stack pages below USER_STACK_TOP
 * Activates the address space, sets TSS.RSP0 to the current kernel
 * stack, and performs SYSRET to enter Ring 3.
 *
 * The user program executes CLI (privileged), triggering #GP which
 * proves privilege isolation works.
 *
 * @note This function does not return in the normal sense.  Once
 *       SYSRET fires, execution continues at Ring 3.
 */
void launch_first_user();

}  // namespace cinux::arch

// ============================================================
// Assembly entry points (C linkage)
// ============================================================

extern "C" {

/**
 * @brief Low-level Ring 0 -> Ring 3 transition via SYSRET
 *
 * This function does not return in the normal sense.  It performs
 * a SYSRET which loads RCX into RIP (user entry), R11 into RFLAGS
 * (with IF=1), and switches to Ring 3 CS/SS selectors.
 *
 * @param entry       Virtual address of the user program entry point
 * @param user_stack  Virtual address of the user stack top
 * @param arg         First argument to pass to the user program (in %rdi)
 */
void jump_to_usermode(uint64_t entry, uint64_t user_stack, uint64_t arg);

}  // extern "C"
