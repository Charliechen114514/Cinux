/**
 * @file kernel/arch/x86_64/gdt.hpp
 * @brief Global Descriptor Table (GDT) for the big kernel
 *
 * Encapsulates GDT state and operations in a class for clean encapsulation
 * and future multi-architecture extensibility.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// Segment Selector Constants
// ============================================================

constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;

// ============================================================
// Segment Descriptor Flags (scoped enum bitmask)
// ============================================================

enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,
    Ring0      = 0u << 5,
    Ring3      = 3u << 5,
    CodeData   = 1u << 4,
    Executable = 1u << 3,
    ReadWrite  = 1u << 1,
    TSS64Avail = 0x09,
};

constexpr SegmentAccess operator|(SegmentAccess a, SegmentAccess b) {
    return static_cast<SegmentAccess>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

enum class SegmentFlags : uint8_t {
    Granularity4K = 1u << 3,
    LongMode      = 1u << 1,
    Size32        = 1u << 2,
};

constexpr SegmentFlags operator|(SegmentFlags a, SegmentFlags b) {
    return static_cast<SegmentFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// ============================================================
// GDT Class
// ============================================================

class GDT {
public:
    void init();

    static void tss_set_rsp0(uint64_t rsp0);

private:
    struct [[gnu::packed]] Entry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  flags_limit_high;
        uint8_t  base_high;
    };
    static_assert(sizeof(Entry) == 8, "GDT entry must be 8 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    /// Minimal TSS placeholder (104 bytes, Intel SDM Vol. 3A Table 8-2)
    struct [[gnu::packed]] TaskStateSegment {
        uint32_t reserved0;
        uint64_t rsp[3];
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    };
    static_assert(sizeof(TaskStateSegment) == 104, "TSS must be 104 bytes");

    /// 4 KB stack for IST1 (Double Fault recovery)
    static constexpr uint64_t DF_STACK_PAGES = 1;
    uint64_t df_stack_phys_{};

    alignas(16) uint8_t df_stack_[DF_STACK_PAGES * 4096]{};

    // Constexpr factory functions
    static constexpr Entry null_entry() { return {0, 0, 0, 0, 0, 0}; }

    static constexpr Entry segment_entry(SegmentAccess access, SegmentFlags flags) {
        return {
            .limit_low        = 0xFFFF,
            .base_low         = 0,
            .base_middle      = 0,
            .access           = static_cast<uint8_t>(access),
            .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
            .base_high        = 0,
        };
    }

    static constexpr Entry tss_low_entry(uint64_t base, uint32_t limit) {
        auto b = static_cast<uint32_t>(base & 0xFFFFFFFF);
        return {
            .limit_low        = static_cast<uint16_t>(limit & 0xFFFF),
            .base_low         = static_cast<uint16_t>(b & 0xFFFF),
            .base_middle      = static_cast<uint8_t>((b >> 16) & 0xFF),
            .access           = static_cast<uint8_t>(SegmentAccess::Present | SegmentAccess::TSS64Avail),
            .flags_limit_high = static_cast<uint8_t>((limit >> 16) & 0x0F),
            .base_high        = static_cast<uint8_t>((b >> 24) & 0xFF),
        };
    }

    static constexpr Entry tss_high_entry(uint64_t base) {
        auto hi = static_cast<uint32_t>(base >> 32);
        return {
            .limit_low        = static_cast<uint16_t>(hi & 0xFFFF),
            .base_low         = static_cast<uint16_t>((hi >> 16) & 0xFFFF),
            .base_middle      = 0,
            .access           = 0,
            .flags_limit_high = 0,
            .base_high        = 0,
        };
    }

    // 5 segment descriptors + TSS (16 bytes = 2 slots) = 7 entries
    static constexpr auto kEntryCount = 7;

    Entry entries_[kEntryCount]{};
    Pointer gdtr_{};
    TaskStateSegment tss_{};

    void load();
};

/// Global GDT instance (zero-initialized in BSS)
extern GDT g_gdt;

}  // namespace cinux::arch
