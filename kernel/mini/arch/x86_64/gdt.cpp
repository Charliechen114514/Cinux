/**
 * @file kernel/mini/arch/x86_64/gdt.cpp
 * @brief GDT Initialization and Loading Implementation
 *
 * Sets up a three-entry GDT (null / code64 / data64) for the mini kernel,
 * loads it via LGDT, then flushes CS through a far jump and reloads all
 * data segment registers.
 */

#include "gdt.hpp"

namespace cinux::mini::arch {

// ============================================================
// Internal State
// ============================================================

/// GDT table instance (three entries: null / code64 / data64)
static GdtEntry s_gdt[GDT_ENTRIES];

/// GDTR pointer structure for loading
static GdtPointer s_gdt_pointer;

// ============================================================
// Internal Helpers
// ============================================================

/**
 * @brief Construct a GDT descriptor entry
 *
 * @param base   Segment base address (ignored in long mode, set to 0)
 * @param limit  Segment limit (ignored in long mode, set to 0xFFFFF)
 * @param access Access permissions byte
 * @param flags  High 4-bit flags (granularity / size / long mode)
 * @return The filled GdtEntry
 *
 * Access Byte layout:
 *   Bit 7   : P (Present)        - 1 = segment is present in memory
 *   Bit 6-5 : DPL (Privilege)    - 00 = ring 0
 *   Bit 4   : S (Descriptor)     - 1 = code/data, 0 = system
 *   Bit 3   : E (Executable)     - 1 = code, 0 = data
 *   Bit 2   : DC (Direction/Conforming)
 *   Bit 1   : RW (Read/Write)
 *   Bit 0   : A (Accessed)       - set automatically by CPU
 *
 * Flags layout:
 *   Bit 3   : G (Granularity)    - 1 = 4KB granularity
 *   Bit 2   : D/B (Default)      - 0 for 64-bit code
 *   Bit 1   : L (Long mode)      - 1 = 64-bit code segment
 *   Bit 0   : Reserved           - 0
 */
static GdtEntry make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    GdtEntry entry;
    entry.limit_low        = limit & 0xFFFF;
    entry.base_low         = base & 0xFFFF;
    entry.base_middle      = (base >> 16) & 0xFF;
    entry.access           = access;
    entry.flags_limit_high = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    entry.base_high        = (base >> 24) & 0xFF;
    return entry;
}

// ============================================================
// Public Interface Implementation
// ============================================================

void gdt_init() {
    // Step 1: Fill null descriptor (index 0, all zeros, unusable)
    s_gdt[GDT_NULL_INDEX] = make_gdt_entry(0, 0, 0, 0);

    // Step 2: Fill 64-bit code segment (index 1)
    // Access: Present=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0 => 0x9A
    // Flags:  G=1, D=0, L=1, Res=0 => 0x0A (long mode code segment)
    s_gdt[GDT_CODE64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);

    // Step 3: Fill 64-bit data segment (index 2)
    // Access: Present=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0 => 0x92
    // Flags:  G=1, D/B=1, L=0, Res=0 => 0x0C (data segment)
    s_gdt[GDT_DATA64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);

    // Step 4: Construct GDTR pointer
    s_gdt_pointer.limit = sizeof(s_gdt) - 1;
    s_gdt_pointer.base  = reinterpret_cast<uint64_t>(&s_gdt);

    // Step 5: Load GDTR and flush all segment registers
    // Inline assembly: lgdt -> far jmp (flush CS) -> reload DS/ES/FS/GS/SS
    __asm__ volatile(
        "lgdt %[gdtr]\n\t"  // Load GDT register

        // Far jump to flush CS segment register
        "pushq %[cs]\n\t"            // Push new code segment selector
        "leaq 1f(%%rip), %%rax\n\t"  // Get address of label 1 as return point
        "pushq %%rax\n\t"            // Push return address
        "lretq\n\t"                  // Far return -> CS is flushed
        "1:\n\t"

        // Reload data segment registers
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(s_gdt_pointer),  // Memory operand, lgdt references directly
          [cs] "i"(SEGMENT_CODE64),   // Immediate, code segment selector
          [ds] "i"(SEGMENT_DATA64)    // Immediate, data segment selector
        : "rax", "memory");
}

}  // namespace cinux::mini::arch
