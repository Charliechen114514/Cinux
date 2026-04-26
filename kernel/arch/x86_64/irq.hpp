/**
 * @file kernel/arch/x86_64/irq.hpp
 * @brief x86 interrupt-flag control primitives
 *
 * Thin inline wrappers around cli / sti / pushfq / popfq.
 * All functions are compiler-barriers ("memory" clobber) so that
 * the compiler does not reorder memory accesses across them.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

#ifdef CINUX_HOST_TEST

namespace cinux::arch {

inline void     irq_disable() {}
inline void     irq_enable() {}
inline uint64_t irq_save() {
    return 0;
}
inline void irq_restore(uint64_t) {}
inline void hlt() {}
inline bool irq_enabled() {
    return true;
}

}  // namespace cinux::arch

#else

namespace cinux::arch {

// ============================================================
// Interrupt enable / disable
// ============================================================

/// Disable maskable interrupts (cli)
inline void irq_disable() {
    __asm__ volatile("cli" ::: "memory");
}

/// Enable maskable interrupts (sti)
inline void irq_enable() {
    __asm__ volatile("sti" ::: "memory");
}

// ============================================================
// Save / restore interrupt state
// ============================================================

/// Return the current RFLAGS and disable interrupts.
/// Pass the returned value to irq_restore() later.
inline uint64_t irq_save() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(flags)::"memory");
    return flags;
}

/// Restore RFLAGS (including the interrupt flag) previously
/// saved by irq_save().
inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" ::"rm"(flags) : "memory");
}

/// Halt until the next interrupt (interrupts must be enabled
/// before calling, or this blocks forever).
inline void hlt() {
    __asm__ volatile("hlt" ::: "memory");
}

/// Check whether interrupts are currently enabled (read RFLAGS.IF).
inline bool irq_enabled() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags));
    return (flags & 0x200) != 0;
}

}  // namespace cinux::arch

#endif
