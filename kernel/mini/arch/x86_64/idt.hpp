/**
 * @file kernel/mini/arch/x86_64/idt.hpp
 * @brief Interrupt Descriptor Table (IDT) - Minimal x86_64 Setup
 *
 * Provides a minimal IDT configuration for the mini kernel,
 * setting up only the required exception vectors:
 *   - #BP (vector 3): Breakpoint exception, triggered by INT3 instruction
 *   - #PF (vector 14): Page fault exception, triggered by page faults
 *
 * Each IDT entry in x86_64 is 16 bytes (128 bits), containing:
 *   - Interrupt handler offset address (split into three parts, 64 bits total)
 *   - Code segment selector (points to the code segment in GDT)
 *   - IST (Interrupt Stack Table) offset
 *   - Type and attributes (Present / DPL / Gate Type)
 *
 * Dependencies: idt_init() must be called after gdt_init().
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::arch {

// ============================================================
// IDT Constants
// ============================================================

/// Maximum number of x86_64 IDT entries (0-255)
constexpr uint16_t IDT_MAX_ENTRIES = 256;

/// Vector numbers configured in this stage
constexpr uint8_t IDT_VEC_BP = 3;   ///< Breakpoint exception
constexpr uint8_t IDT_VEC_PF = 14;  ///< Page fault exception

/// IDT entry type: 64-bit interrupt gate (no error code pushed, handled by ISR)
constexpr uint8_t IDT_TYPE_INTERRUPT_GATE = 0x0E;
/// IDT entry type: 64-bit trap gate (differs from interrupt gate by not clearing IF)
constexpr uint8_t IDT_TYPE_TRAP_GATE      = 0x0F;

// ============================================================
// IDT Descriptor Structure (16 bytes)
// ============================================================

/**
 * @brief 64-bit IDT gate descriptor
 *
 * Each IDT entry occupies 16 bytes, containing the full address of the
 * interrupt handler, segment selector, IST offset, and type/permission attributes.
 */
struct IdtEntry {
    uint16_t offset_low;   ///< Handler address low 16 bits [0:15]
    uint16_t selector;     ///< Code segment selector (CS)
    uint8_t  ist;          ///< IST offset (0 = do not use IST)
    uint8_t  type_attr;    ///< Type and attributes (P | DPL | 0 | Gate Type)
    uint16_t offset_mid;   ///< Handler address middle 16 bits [16:31]
    uint32_t offset_high;  ///< Handler address high 32 bits [32:63]
    uint32_t reserved;     ///< Reserved, must be 0
} __attribute__((packed));

/**
 * @brief IDT register structure (for the LIDT instruction)
 *
 * Corresponds to the x86 LIDT instruction operand format:
 * 2-byte limit + 8-byte base address.
 */
struct IdtPointer {
    uint16_t limit;  ///< IDT byte size - 1
    uint64_t base;   ///< Linear address of the IDT
} __attribute__((packed));

// ============================================================
// Interrupt Stack Frame (pushed by CPU on exception/interrupt)
// ============================================================

/**
 * @brief x86_64 interrupt stack frame
 *
 * When the CPU responds to an exception/interrupt, it automatically pushes
 * the following registers onto the stack. If the exception produces an error
 * code (e.g. #PF), the CPU additionally pushes it last.
 * The ISR stub passes a pointer to this structure as the first argument
 * before jumping to the C handler function.
 *
 * Note: For exceptions without an error code (e.g. #BP), the ISR stub
 *       pushes a dummy error code (0) to keep the stack frame aligned.
 */
struct InterruptFrame {
    uint64_t r15, r14, r13, r12;  ///< General-purpose registers (saved by ISR stub)
    uint64_t r11, r10, r9, r8;    ///< General-purpose registers (saved by ISR stub)
    uint64_t rdi, rsi, rbp, rdx;  ///< General-purpose registers (saved by ISR stub)
    uint64_t rcx, rbx, rax;       ///< General-purpose registers (saved by ISR stub)
    uint64_t error_code;          ///< Error code (stub fills 0 for exceptions without one)
    uint64_t rip;                 ///< Instruction pointer (pushed by CPU)
    uint64_t cs;                  ///< Code segment selector (pushed by CPU)
    uint64_t rflags;              ///< Flags register (pushed by CPU)
    uint64_t rsp;                 ///< Stack pointer (pushed by CPU)
    uint64_t ss;                  ///< Stack segment selector (pushed by CPU)
};

// ============================================================
// Public Interface
// ============================================================

/**
 * @brief Initialize and load the IDT
 *
 * Clears all 256 IDT entries, then configures the #BP(3) and #PF(14)
 * exception vectors, and finally executes LIDT to load it.
 * ISR stub addresses come from symbols defined in interrupts.S.
 *
 * @note Must be called after gdt_init(), because the selector field
 *       in IDT entries references the code segment selector in the GDT.
 */
void idt_init();

}  // namespace cinux::mini::arch
