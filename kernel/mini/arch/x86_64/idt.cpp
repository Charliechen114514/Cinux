/**
 * @file kernel/mini/arch/x86_64/idt.cpp
 * @brief IDT Initialization and Loading Implementation
 *
 * Configures IDT entries for the #BP(3) and #PF(14) exception vectors,
 * and loads them into the CPU via the LIDT instruction.
 */

#include "idt.hpp"

#include "gdt.hpp"

namespace cinux::mini::arch {

// ============================================================
// Internal State
// ============================================================

/// IDT table instance (256 entries, mostly empty)
static IdtEntry s_idt[IDT_MAX_ENTRIES];

/// IDTR pointer structure for loading
static IdtPointer s_idt_pointer;

// ============================================================
// ISR stub declarations (defined in interrupts.S)
// ============================================================

/// ISR stub entry for #BP(3)
extern "C" void isr_bp_stub();
/// ISR stub entry for #PF(14)
extern "C" void isr_pf_stub();

// ============================================================
// Exception handler declarations (defined in exception_handlers.cpp)
// ============================================================

extern "C" void handle_bp(InterruptFrame* frame);
extern "C" void handle_pf(InterruptFrame* frame);

// ============================================================
// Internal Helpers
// ============================================================

/**
 * @brief Construct an IDT entry
 *
 * @param vector   Vector number (0-255)
 * @param handler  Address of the interrupt handler
 * @param selector Code segment selector (typically SEGMENT_CODE64)
 * @param type_attr Type and attribute byte
 *        - Bit 7:   P (Present) = 1
 *        - Bit 6-5: DPL (Descriptor Privilege Level) = 00 (ring 0)
 *        - Bit 4:   0 (fixed)
 *        - Bit 3-0: Gate Type (0xE = interrupt, 0xF = trap)
 * @param ist IST offset (0 = do not use IST)
 */
static void set_idt_entry(uint8_t vector, void* handler, uint16_t selector, uint8_t type_attr,
                          uint8_t ist) {
    uint64_t addr = reinterpret_cast<uint64_t>(handler);

    s_idt[vector].offset_low  = addr & 0xFFFF;
    s_idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    s_idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;

    s_idt[vector].selector  = selector;
    s_idt[vector].ist       = ist;
    s_idt[vector].type_attr = type_attr;
    s_idt[vector].reserved  = 0;
}

// ============================================================
// Public Interface Implementation
// ============================================================

void idt_init() {
    // Step 1: Clear all IDT entries (zeroed out, Present=0 means unused)
    for (uint16_t i = 0; i < IDT_MAX_ENTRIES; i++) {
        s_idt[i] = IdtEntry{};
    }

    // Step 2: Configure #BP(3) - Breakpoint exception
    // Use a Trap Gate so IF is not cleared upon handler entry,
    // allowing other interrupts to be serviced during breakpoint handling.
    // DPL = 3 allows INT3 from user mode (though we only have ring 0 for now).
    // type_attr = 0x8F: P=1, DPL=00, Type=0xF (trap gate)
    set_idt_entry(IDT_VEC_BP, reinterpret_cast<void*>(isr_bp_stub), SEGMENT_CODE64,
                  0x8F,  // Present | DPL=0 | Trap Gate
                  0);    // Do not use IST

    // Step 3: Configure #PF(14) - Page fault exception
    // Use an Interrupt Gate, which clears IF on entry.
    // #PF automatically pushes an error code; the ISR stub must handle it.
    // type_attr = 0x8E: P=1, DPL=00, Type=0xE (interrupt gate)
    set_idt_entry(IDT_VEC_PF, reinterpret_cast<void*>(isr_pf_stub), SEGMENT_CODE64,
                  0x8E,  // Present | DPL=0 | Interrupt Gate
                  0);    // Do not use IST

    // Step 4: Construct IDTR pointer
    s_idt_pointer.limit = static_cast<uint16_t>(sizeof(s_idt) - 1);
    s_idt_pointer.base  = reinterpret_cast<uint64_t>(&s_idt);

    // Step 5: Load IDTR
    __asm__ volatile("lidt %[idtr]\n\t" : : [idtr] "m"(s_idt_pointer) : "memory");
}

}  // namespace cinux::mini::arch
