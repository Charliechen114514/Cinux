/**
 * @file kernel/arch/x86_64/idt.hpp
 * @brief Interrupt Descriptor Table (IDT) for the big kernel
 *
 * Encapsulates IDT management in a class. Exception vectors, gate types,
 * and privilege levels are expressed through scoped enums used as first-class
 * API types.
 *
 * Gate type policy (design decision):
 *   - #BP(3) / #DB(1): Trap Gate (IF preserved)
 *   - All other exceptions: Interrupt Gate (IF cleared)
 *
 * Dependencies: IDT::init() must be called after GDT::init().
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// Exception Vectors (scoped enum)
// ============================================================

enum class ExceptionVector : uint8_t {
    DE  = 0,   ///< #DE: Divide Error
    DB  = 1,   ///< #DB: Debug Exception
    NMI = 2,   ///< Non-maskable Interrupt
    BP  = 3,   ///< #BP: Breakpoint (INT3)
    OF  = 4,   ///< #OF: Overflow
    BR  = 5,   ///< #BR: BOUND Range Exceeded
    UD  = 6,   ///< #UD: Invalid Opcode
    NM  = 7,   ///< #NM: Device Not Available
    DF  = 8,   ///< #DF: Double Fault (has error code)
    TS  = 10,  ///< #TS: Invalid TSS (has error code)
    NP  = 11,  ///< #NP: Segment Not Present (has error code)
    SS  = 12,  ///< #SS: Stack-Segment Fault (has error code)
    GP  = 13,  ///< #GP: General Protection (has error code)
    PF  = 14,  ///< #PF: Page Fault (has error code)
};

// ============================================================
// IDT Gate Types and Privilege Levels
// ============================================================

enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,  ///< 64-bit interrupt gate (clears IF)
    Trap      = 0x0F,  ///< 64-bit trap gate (preserves IF)
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,  ///< Ring 0 only
    User   = 0x60,  ///< Ring 3 (DPL=3)
};

/// Build a type_attr byte from privilege and gate type
constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}

// ============================================================
// Interrupt Stack Frame
// ============================================================

/// x86_64 interrupt stack frame (pushed by ISR stub + CPU)
struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

// ============================================================
// IDT Class
// ============================================================

class IDT {
public:
    /// C handler function signature
    using Handler = void (*)(InterruptFrame*);

    /// Assembly ISR stub signature
    using Stub = void (*)();

    void init();
    void set_handler(ExceptionVector vector, Stub stub, uint16_t selector, uint8_t type_attr,
                     uint8_t ist = 0);

private:
    struct [[gnu::packed]] Entry {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    };
    static_assert(sizeof(Entry) == 16, "IDT entry must be 16 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    static constexpr uint16_t kMaxEntries = 256;

    Entry   entries_[kMaxEntries]{};
    Pointer idtr_{};

    void load();
};

/// Global IDT instance (zero-initialized in BSS)
extern IDT g_idt;

}  // namespace cinux::arch
