/**
 * @file kernel/mini/arch/x86_64/gdt.hpp
 * @brief Global Descriptor Table (GDT) - Minimal x86_64 Setup
 *
 * Provides the most basic GDT configuration for the mini kernel:
 * null descriptor, 64-bit code segment, and 64-bit data segment.
 * In x86_64, segmentation is largely deprecated; the GDT is mainly used for:
 *   - Setting base addresses and permissions for segment registers (CS/DS/ES/SS)
 *   - Providing the required code/data segment descriptors for long mode
 *   - ISR/IRETQ requires correct segment selectors to restore context
 *
 * Call order: init() must complete before any interrupt/exception handling.
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::arch {

// ============================================================
// GDT Constants
// ============================================================

/// Number of GDT entries: null + code64 + data64 = 3
constexpr uint8_t GDT_ENTRIES = 3;

/// GDT entry indices (also serve as the TI=0 portion of segment selectors, in units of 8 bytes)
constexpr uint8_t GDT_NULL_INDEX   = 0;
constexpr uint8_t GDT_CODE64_INDEX = 1;
constexpr uint8_t GDT_DATA64_INDEX = 2;

/// Segment selectors: index * 8 + RPL (Requested Privilege Level)
/// All selectors use RPL=0 (ring 0 kernel mode)
constexpr uint16_t SEGMENT_NULL   = GDT_NULL_INDEX * 8;
constexpr uint16_t SEGMENT_CODE64 = GDT_CODE64_INDEX * 8;
constexpr uint16_t SEGMENT_DATA64 = GDT_DATA64_INDEX * 8;

// ============================================================
// GDT Descriptor Structures (10 bytes: 8-byte descriptor + 2-byte limit low 16 bits)
// ============================================================

/**
 * @brief 64-bit GDT descriptor (8 bytes)
 *
 * Segment descriptor format for x86_64 long mode, compatible with the
 * legacy 32-bit format. The base and limit fields are ignored by hardware
 * in long mode (except for GS/FS base). The key fields are the Access Byte
 * and the granularity/size bits in Flags.
 */
struct GdtEntry {
    uint16_t limit_low;         ///< Segment limit low 16 bits
    uint16_t base_low;          ///< Base address low 16 bits
    uint8_t  base_middle;       ///< Base address middle 8 bits
    uint8_t  access;            ///< Access byte (Type + DPL + P, etc.)
    uint8_t  flags_limit_high;  ///< High 4 bits flags + low 4 bits limit high 4 bits
    uint8_t  base_high;         ///< Base address high 8 bits
} __attribute__((packed));

/**
 * @brief GDT register structure (for the LGDT instruction)
 *
 * Corresponds to the x86 LGDT instruction operand format:
 * 2-byte limit + 8-byte base address. limit = total GDT bytes - 1.
 */
struct GdtPointer {
    uint16_t limit;  ///< GDT byte size - 1
    uint64_t base;   ///< Linear address of the GDT
} __attribute__((packed));

// ============================================================
// Public Interface
// ============================================================

/**
 * @brief Initialize and load the GDT
 *
 * Fills in the three GDT entries (null / code64 / data64), constructs
 * the GdtPointer, executes LGDT to load it, then flushes all segment
 * registers (CS/DS/ES/FS/GS/SS) to point to the new GDT entries.
 *
 * @note Must be called before enabling interrupts. After the call,
 *       CS = SEGMENT_CODE64, DS/ES/FS/GS/SS = SEGMENT_DATA64.
 */
void gdt_init();

}  // namespace cinux::mini::arch
