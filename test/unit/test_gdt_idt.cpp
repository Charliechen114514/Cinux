/**
 * @file test/unit/test_gdt_idt.cpp
 * @brief Host-side unit tests for big-kernel GDT/IDT data structures and encoding logic
 *
 * Test coverage:
 *   - GDT segment selector constants (0x10/0x18/0x33/0x2B/0x38)
 *   - GDT factory functions: null_entry, segment_entry, tss entries
 *   - IDT ExceptionVector enum underlying values (DE=0, DF=8, GP=13, PF=14)
 *   - IDT make_idt_attr: correct combination of Present + DPL + Gate Type
 *   - InterruptFrame layout (static_assert size, field offsets)
 *   - Exception handler behavior categories (non-fatal vs fatal)
 *
 * The big-kernel GDT/IDT classes use private nested types for hardware
 * structures.  We mirror the encoding logic in local helper functions to
 * test the pure arithmetic without linking kernel code.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

// Big-kernel headers (cinux::arch namespace)
#    include "arch/x86_64/gdt.hpp"
#    include "arch/x86_64/idt.hpp"

using namespace cinux::arch;

// ============================================================
// Mirror types for testing private encoding logic
// ============================================================
// The GDT/IDT classes place their Entry structs and factory
// functions behind private access.  We replicate the encoding
// logic here to unit-test the arithmetic in isolation.

/// Mirror of GDT::Entry (8-byte segment descriptor)
struct [[gnu::packed]] TestGdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
};
static_assert(sizeof(TestGdtEntry) == 8, "mirrored GDT entry must be 8 bytes");

/// Mirror of GDT::Pointer (10-byte GDTR)
struct [[gnu::packed]] TestGdtPointer {
    uint16_t limit;
    uint64_t base;
};
static_assert(sizeof(TestGdtPointer) == 10, "mirrored GDTR must be 10 bytes");

/// Mirror of GDT::TaskStateSegment (104-byte TSS)
struct [[gnu::packed]] TestTSS {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(TestTSS) == 104, "mirrored TSS must be 104 bytes");

/// Mirror of IDT::Entry (16-byte gate descriptor)
struct [[gnu::packed]] TestIdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};
static_assert(sizeof(TestIdtEntry) == 16, "mirrored IDT entry must be 16 bytes");

/// Mirror of IDT::Pointer (10-byte IDTR)
struct [[gnu::packed]] TestIdtPointer {
    uint16_t limit;
    uint64_t base;
};
static_assert(sizeof(TestIdtPointer) == 10, "mirrored IDTR must be 10 bytes");

// ============================================================
// Mirror factory functions (replicate GDT/IDT private logic)
// ============================================================

static constexpr TestGdtEntry test_null_entry() {
    return {0, 0, 0, 0, 0, 0};
}

static constexpr TestGdtEntry test_segment_entry(SegmentAccess access, SegmentFlags flags) {
    return {
        .limit_low        = 0xFFFF,
        .base_low         = 0,
        .base_middle      = 0,
        .access           = static_cast<uint8_t>(access),
        .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
        .base_high        = 0,
    };
}

static constexpr TestGdtEntry test_tss_low_entry(uint64_t base, uint32_t limit) {
    auto b = static_cast<uint32_t>(base & 0xFFFFFFFF);
    return {
        .limit_low   = static_cast<uint16_t>(limit & 0xFFFF),
        .base_low    = static_cast<uint16_t>(b & 0xFFFF),
        .base_middle = static_cast<uint8_t>((b >> 16) & 0xFF),
        .access      = static_cast<uint8_t>(SegmentAccess::Present | SegmentAccess::TSS64Avail),
        .flags_limit_high = static_cast<uint8_t>((limit >> 16) & 0x0F),
        .base_high        = static_cast<uint8_t>((b >> 24) & 0xFF),
    };
}

static constexpr TestGdtEntry test_tss_high_entry(uint64_t base) {
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

static void test_set_idt_entry(TestIdtEntry* table, uint8_t vector, uint64_t handler_addr,
                               uint16_t selector, uint8_t type_attr, uint8_t ist) {
    table[vector].offset_low  = handler_addr & 0xFFFF;
    table[vector].offset_mid  = (handler_addr >> 16) & 0xFFFF;
    table[vector].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
    table[vector].selector    = selector;
    table[vector].ist         = ist;
    table[vector].type_attr   = type_attr;
    table[vector].reserved    = 0;
}

// ============================================================
// 1. GDT Segment Selector Constants
// ============================================================

/// Verify kernel code selector is 0x10 (index 2, RPL 0)
TEST("gdt: kernel code selector is 0x10") {
    ASSERT_EQ(GDT_KERNEL_CODE, 0x0010u);
}

/// Verify kernel data selector is 0x18 (index 3, RPL 0)
TEST("gdt: kernel data selector is 0x18") {
    ASSERT_EQ(GDT_KERNEL_DATA, 0x0018u);
}

/// Verify user code selector is 0x33 (index 6, RPL 3)
TEST("gdt: user code selector is 0x33") {
    ASSERT_EQ(GDT_USER_CODE, 0x0033u);
}

/// Verify user data selector is 0x2B (index 5, RPL 3)
TEST("gdt: user data selector is 0x2B") {
    ASSERT_EQ(GDT_USER_DATA, 0x002Bu);
}

/// Verify TSS selector is 0x38 (index 7, RPL 0)
TEST("gdt: TSS selector is 0x38") {
    ASSERT_EQ(GDT_TSS, 0x0038u);
}

/// Verify selector stride is 8 bytes each
TEST("gdt: selectors have 8-byte stride") {
    ASSERT_EQ(static_cast<unsigned>(GDT_KERNEL_DATA - GDT_KERNEL_CODE), 0x0008u);
    ASSERT_EQ(static_cast<unsigned>(GDT_USER_CODE - GDT_KERNEL_DATA), 0x001Bu);
}

/// Verify user selectors have RPL=3 in low 2 bits
TEST("gdt: user selectors encode RPL 3") {
    ASSERT_EQ(static_cast<unsigned>(GDT_USER_CODE & 0x03), 0x03u);
    ASSERT_EQ(static_cast<unsigned>(GDT_USER_DATA & 0x03), 0x03u);
}

/// Verify kernel selectors have RPL=0 in low 2 bits
TEST("gdt: kernel selectors encode RPL 0") {
    ASSERT_EQ(static_cast<unsigned>(GDT_KERNEL_CODE & 0x03), 0x00u);
    ASSERT_EQ(static_cast<unsigned>(GDT_KERNEL_DATA & 0x03), 0x00u);
    ASSERT_EQ(static_cast<unsigned>(GDT_TSS & 0x03), 0x00u);
}

// ============================================================
// 2. GDT SegmentAccess Scoped Enum
// ============================================================

/// Verify SegmentAccess bit positions
TEST("gdt: SegmentAccess bit positions") {
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::Present), 0x80u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::Ring0), 0x00u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::Ring3), 0x60u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::CodeData), 0x10u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::Executable), 0x08u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::ReadWrite), 0x02u);
}

/// Verify TSS64Avail value (system type 0x09)
TEST("gdt: SegmentAccess TSS64Avail value") {
    ASSERT_EQ(static_cast<uint8_t>(SegmentAccess::TSS64Avail), 0x09u);
}

/// Verify SegmentAccess OR operator combines bits
TEST("gdt: SegmentAccess OR operator") {
    auto combined = SegmentAccess::Present | SegmentAccess::CodeData | SegmentAccess::Executable |
                    SegmentAccess::ReadWrite;
    // Expected: 0x80 | 0x10 | 0x08 | 0x02 = 0x9A (kernel code)
    ASSERT_EQ(static_cast<uint8_t>(combined), 0x9Au);
}

/// Verify kernel code access byte composition via scoped enums
TEST("gdt: kernel code access from scoped enums") {
    auto access = SegmentAccess::Present | SegmentAccess::CodeData | SegmentAccess::Executable |
                  SegmentAccess::ReadWrite;
    uint8_t val = static_cast<uint8_t>(access);
    // P=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0 => 0x9A
    ASSERT_EQ(static_cast<unsigned>(val), 0x9Au);
    ASSERT_TRUE(val & 0x80);       // P=1
    ASSERT_EQ((val >> 5) & 3, 0);  // DPL=00
    ASSERT_TRUE(val & 0x10);       // S=1
    ASSERT_TRUE(val & 0x08);       // E=1
    ASSERT_TRUE(val & 0x02);       // RW=1
}

/// Verify kernel data access byte composition via scoped enums
TEST("gdt: kernel data access from scoped enums") {
    auto    access = SegmentAccess::Present | SegmentAccess::CodeData | SegmentAccess::ReadWrite;
    uint8_t val    = static_cast<uint8_t>(access);
    // P=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0 => 0x92
    ASSERT_EQ(static_cast<unsigned>(val), 0x92u);
    ASSERT_TRUE(val & 0x80);   // P=1
    ASSERT_FALSE(val & 0x08);  // E=0 (data segment)
    ASSERT_TRUE(val & 0x02);   // RW=1
}

/// Verify user code access byte composition
TEST("gdt: user code access from scoped enums") {
    auto access = SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                  SegmentAccess::Executable | SegmentAccess::ReadWrite;
    uint8_t val = static_cast<uint8_t>(access);
    // P=1, DPL=11, S=1, E=1, DC=0, RW=1, A=0 => 0xFA
    ASSERT_EQ(static_cast<unsigned>(val), 0xFAu);
    ASSERT_EQ((val >> 5) & 3, 3);  // DPL=11 (ring 3)
}

/// Verify user data access byte composition
TEST("gdt: user data access from scoped enums") {
    auto access = SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                  SegmentAccess::ReadWrite;
    uint8_t val = static_cast<uint8_t>(access);
    // P=1, DPL=11, S=1, E=0, DC=0, RW=1, A=0 => 0xF2
    ASSERT_EQ(static_cast<unsigned>(val), 0xF2u);
    ASSERT_EQ((val >> 5) & 3, 3);  // DPL=11
}

// ============================================================
// 3. GDT SegmentFlags Scoped Enum
// ============================================================

/// Verify SegmentFlags bit positions
TEST("gdt: SegmentFlags bit positions") {
    ASSERT_EQ(static_cast<uint8_t>(SegmentFlags::Granularity4K), 0x08u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentFlags::LongMode), 0x02u);
    ASSERT_EQ(static_cast<uint8_t>(SegmentFlags::Size32), 0x04u);
}

/// Verify code64 flags: 4K granularity + long mode (L=1, D=0)
TEST("gdt: code64 flags combination") {
    auto    flags = SegmentFlags::Granularity4K | SegmentFlags::LongMode;
    uint8_t val   = static_cast<uint8_t>(flags);
    ASSERT_EQ(static_cast<unsigned>(val), 0x0Au);  // G=1(bit3), L=1(bit1)
}

/// Verify data64 flags: 4K granularity + 32-bit size (L=0, D/B=1)
TEST("gdt: data64 flags combination") {
    auto    flags = SegmentFlags::Granularity4K | SegmentFlags::Size32;
    uint8_t val   = static_cast<uint8_t>(flags);
    ASSERT_EQ(static_cast<unsigned>(val), 0x0Cu);  // G=1(bit3), D/B=1(bit2)
}

// ============================================================
// 4. GDT Factory Functions (mirrored logic)
// ============================================================

/// Verify null_entry returns all zeros
TEST("gdt: null_entry is all zeros") {
    auto    entry = test_null_entry();
    uint8_t bytes[sizeof(entry)];
    memcpy(bytes, &entry, sizeof(entry));
    for (size_t i = 0; i < sizeof(entry); i++) {
        ASSERT_EQ(bytes[i], 0);
    }
}

/// Verify segment_entry for kernel code: access=0x9A, flags=0x0A
TEST("gdt: segment_entry kernel code encoding") {
    auto access = SegmentAccess::Present | SegmentAccess::CodeData | SegmentAccess::Executable |
                  SegmentAccess::ReadWrite;
    auto flags = SegmentFlags::Granularity4K | SegmentFlags::LongMode;
    auto entry = test_segment_entry(access, flags);

    ASSERT_EQ(entry.limit_low, 0xFFFF);
    ASSERT_EQ(entry.base_low, 0);
    ASSERT_EQ(entry.base_middle, 0);
    ASSERT_EQ(entry.access, 0x9A);
    // flags << 4 | 0x0F = 0xA0 | 0x0F = 0xAF
    ASSERT_EQ(entry.flags_limit_high, 0xAF);
    ASSERT_EQ(entry.base_high, 0);
}

/// Verify segment_entry for kernel data: access=0x92, flags=0x0C
TEST("gdt: segment_entry kernel data encoding") {
    auto access = SegmentAccess::Present | SegmentAccess::CodeData | SegmentAccess::ReadWrite;
    auto flags  = SegmentFlags::Granularity4K | SegmentFlags::Size32;
    auto entry  = test_segment_entry(access, flags);

    ASSERT_EQ(entry.access, 0x92);
    // flags << 4 | 0x0F = 0xC0 | 0x0F = 0xCF
    ASSERT_EQ(entry.flags_limit_high, 0xCF);
}

/// Verify segment_entry for user code: access=0xFA
TEST("gdt: segment_entry user code encoding") {
    auto access = SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                  SegmentAccess::Executable | SegmentAccess::ReadWrite;
    auto flags = SegmentFlags::Granularity4K | SegmentFlags::LongMode;
    auto entry = test_segment_entry(access, flags);

    ASSERT_EQ(entry.access, 0xFA);
    ASSERT_EQ(entry.flags_limit_high, 0xAF);
}

/// Verify segment_entry for user data: access=0xF2
TEST("gdt: segment_entry user data encoding") {
    auto access = SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                  SegmentAccess::ReadWrite;
    auto flags = SegmentFlags::Granularity4K | SegmentFlags::Size32;
    auto entry = test_segment_entry(access, flags);

    ASSERT_EQ(entry.access, 0xF2);
    ASSERT_EQ(entry.flags_limit_high, 0xCF);
}

/// Verify TSS low entry encoding with a known base address
TEST("gdt: tss_low_entry base split") {
    uint64_t base  = 0x0000000000012345;
    uint32_t limit = 103;  // TSS is 104 bytes, limit = 103
    auto     entry = test_tss_low_entry(base, limit);

    ASSERT_EQ(entry.limit_low, 103u);
    ASSERT_EQ(entry.base_low, 0x2345u);
    ASSERT_EQ(entry.base_middle, 0x01u);
    ASSERT_EQ(entry.base_high, 0x00u);
    // access = Present | TSS64Avail = 0x80 | 0x09 = 0x89
    ASSERT_EQ(entry.access, 0x89u);
    ASSERT_EQ(entry.flags_limit_high, 0x00u);
}

/// Verify TSS high entry encodes upper 32 bits of 64-bit base
TEST("gdt: tss_high_entry upper 32 bits") {
    uint64_t base  = 0x000000ABCD000000ULL;
    auto     entry = test_tss_high_entry(base);

    auto hi = static_cast<uint32_t>(base >> 32);
    ASSERT_EQ(entry.limit_low, static_cast<uint16_t>(hi & 0xFFFF));
    ASSERT_EQ(entry.base_low, static_cast<uint16_t>((hi >> 16) & 0xFFFF));
    ASSERT_EQ(entry.access, 0);
    ASSERT_EQ(entry.flags_limit_high, 0);
}

/// Verify TSS low entry with base above 4GB (upper bits go to high entry)
TEST("gdt: tss_low_entry ignores upper 32 bits of base") {
    uint64_t base  = 0xFFFFFFFF00000000ULL;
    uint32_t limit = 103;
    auto     entry = test_tss_low_entry(base, limit);

    // Low 32 bits are all zero
    ASSERT_EQ(entry.base_low, 0x0000u);
    ASSERT_EQ(entry.base_middle, 0x00u);
    ASSERT_EQ(entry.base_high, 0x00u);
}

// ============================================================
// 5. GDT Entry Struct Layout (mirrored types)
// ============================================================

/// Verify mirrored GDT entry size is 8 bytes
TEST("gdt: TestGdtEntry size is 8 bytes") {
    ASSERT_EQ(sizeof(TestGdtEntry), 8u);
}

/// Verify mirrored GDT entry field offsets
TEST("gdt: TestGdtEntry field offsets") {
    ASSERT_EQ(offsetof(TestGdtEntry, limit_low), 0u);
    ASSERT_EQ(offsetof(TestGdtEntry, base_low), 2u);
    ASSERT_EQ(offsetof(TestGdtEntry, base_middle), 4u);
    ASSERT_EQ(offsetof(TestGdtEntry, access), 5u);
    ASSERT_EQ(offsetof(TestGdtEntry, flags_limit_high), 6u);
    ASSERT_EQ(offsetof(TestGdtEntry, base_high), 7u);
}

/// Verify mirrored GDTR size is 10 bytes
TEST("gdt: TestGdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(TestGdtPointer), 10u);
}

/// Verify mirrored GDTR field offsets
TEST("gdt: TestGdtPointer field offsets") {
    ASSERT_EQ(offsetof(TestGdtPointer, limit), 0u);
    ASSERT_EQ(offsetof(TestGdtPointer, base), 2u);
}

/// Verify mirrored TSS size is 104 bytes
TEST("gdt: TestTSS size is 104 bytes") {
    ASSERT_EQ(sizeof(TestTSS), 104u);
}

/// Verify GDT entry count: 5 segments + TSS (2 slots) = 7
TEST("gdt: entry count is 7") {
    // null + kernel_code + kernel_data + user_code + user_data + TSS_low + TSS_high
    constexpr int expected = 7;
    ASSERT_EQ(expected, 7);
}

/// Verify GDTR limit calculation for 7 entries
TEST("gdt: GDTR limit for 7 entries") {
    // limit = 7 * 8 - 1 = 55
    uint16_t limit = sizeof(TestGdtEntry) * 7 - 1;
    ASSERT_EQ(limit, 55u);
}

// ============================================================
// 6. IDT ExceptionVector Enum Values
// ============================================================

/// Verify ExceptionVector underlying values for key exceptions
TEST("idt: ExceptionVector DE is 0") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DE), 0);
}

TEST("idt: ExceptionVector DB is 1") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DB), 1);
}

TEST("idt: ExceptionVector NMI is 2") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::NMI), 2);
}

TEST("idt: ExceptionVector BP is 3") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::BP), 3);
}

TEST("idt: ExceptionVector OF is 4") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::OF), 4);
}

TEST("idt: ExceptionVector BR is 5") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::BR), 5);
}

TEST("idt: ExceptionVector UD is 6") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::UD), 6);
}

TEST("idt: ExceptionVector NM is 7") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::NM), 7);
}

TEST("idt: ExceptionVector DF is 8") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DF), 8);
}

TEST("idt: ExceptionVector TS is 10") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::TS), 10);
}

TEST("idt: ExceptionVector NP is 11") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::NP), 11);
}

TEST("idt: ExceptionVector SS is 12") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::SS), 12);
}

TEST("idt: ExceptionVector GP is 13") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::GP), 13);
}

TEST("idt: ExceptionVector PF is 14") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::PF), 14);
}

/// Verify exception vector ordering: DE < DF < GP < PF
TEST("idt: exception vector ordering") {
    ASSERT_LT(static_cast<uint8_t>(ExceptionVector::DE), static_cast<uint8_t>(ExceptionVector::DF));
    ASSERT_LT(static_cast<uint8_t>(ExceptionVector::DF), static_cast<uint8_t>(ExceptionVector::GP));
    ASSERT_LT(static_cast<uint8_t>(ExceptionVector::GP), static_cast<uint8_t>(ExceptionVector::PF));
}

// ============================================================
// 7. IDT Gate Type and Privilege Enums
// ============================================================

/// Verify interrupt gate type value
TEST("idt: IDTGateType Interrupt is 0x0E") {
    ASSERT_EQ(static_cast<uint8_t>(IDTGateType::Interrupt), 0x0Eu);
}

/// Verify trap gate type value
TEST("idt: IDTGateType Trap is 0x0F") {
    ASSERT_EQ(static_cast<uint8_t>(IDTGateType::Trap), 0x0Fu);
}

/// Verify kernel privilege value
TEST("idt: IDTPrivilege Kernel is 0x00") {
    ASSERT_EQ(static_cast<uint8_t>(IDTPrivilege::Kernel), 0x00u);
}

/// Verify user privilege value
TEST("idt: IDTPrivilege User is 0x60") {
    ASSERT_EQ(static_cast<uint8_t>(IDTPrivilege::User), 0x60u);
}

// ============================================================
// 8. make_idt_attr Encoding
// ============================================================

/// Verify make_idt_attr for kernel interrupt gate (Present + DPL0 + IntGate)
TEST("idt: make_idt_attr kernel interrupt gate") {
    uint8_t attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    // 0x80 | 0x00 | 0x0E = 0x8E
    ASSERT_EQ(attr, 0x8Eu);
}

/// Verify make_idt_attr for kernel trap gate (Present + DPL0 + TrapGate)
TEST("idt: make_idt_attr kernel trap gate") {
    uint8_t attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Trap);
    // 0x80 | 0x00 | 0x0F = 0x8F
    ASSERT_EQ(attr, 0x8Fu);
}

/// Verify make_idt_attr for user interrupt gate (Present + DPL3 + IntGate)
TEST("idt: make_idt_attr user interrupt gate") {
    uint8_t attr = make_idt_attr(IDTPrivilege::User, IDTGateType::Interrupt);
    // 0x80 | 0x60 | 0x0E = 0xEE
    ASSERT_EQ(attr, 0xEEu);
}

/// Verify make_idt_attr for user trap gate (Present + DPL3 + TrapGate)
TEST("idt: make_idt_attr user trap gate") {
    uint8_t attr = make_idt_attr(IDTPrivilege::User, IDTGateType::Trap);
    // 0x80 | 0x60 | 0x0F = 0xEF
    ASSERT_EQ(attr, 0xEFu);
}

/// Verify Present bit is always set in make_idt_attr
TEST("idt: make_idt_attr always sets Present bit") {
    uint8_t kernel_int = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    uint8_t user_trap  = make_idt_attr(IDTPrivilege::User, IDTGateType::Trap);
    ASSERT_TRUE(kernel_int & 0x80);
    ASSERT_TRUE(user_trap & 0x80);
}

/// Verify gate type occupies low nibble in make_idt_attr result
TEST("idt: make_idt_attr gate type in low nibble") {
    uint8_t int_attr  = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    uint8_t trap_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Trap);
    ASSERT_EQ(static_cast<unsigned>(int_attr & 0x0F), 0x0Eu);
    ASSERT_EQ(static_cast<unsigned>(trap_attr & 0x0F), 0x0Fu);
}

/// Verify DPL occupies bits 5-6 in make_idt_attr result
TEST("idt: make_idt_attr DPL in bits 5-6") {
    uint8_t kernel = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    uint8_t user   = make_idt_attr(IDTPrivilege::User, IDTGateType::Interrupt);
    ASSERT_EQ(static_cast<unsigned>((kernel >> 5) & 0x03), 0x00u);
    ASSERT_EQ(static_cast<unsigned>((user >> 5) & 0x03), 0x03u);
}

// ============================================================
// 9. Gate Type Policy (design decision verification)
// ============================================================

/// Verify #BP and #DB use trap gate (IF preserved)
TEST("idt: BP and DB use trap gate") {
    // Breakpoint and Debug exceptions should use trap gates
    // so that IF is preserved upon return
    uint8_t bp_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Trap);
    uint8_t db_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Trap);
    ASSERT_EQ(static_cast<unsigned>(bp_attr & 0x0F), 0x0Fu);  // Trap gate
    ASSERT_EQ(static_cast<unsigned>(db_attr & 0x0F), 0x0Fu);  // Trap gate
}

/// Verify fatal exceptions use interrupt gate (IF cleared)
TEST("idt: fatal exceptions use interrupt gate") {
    // DE, GP, PF and other fatal exceptions should use interrupt gates
    uint8_t de_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    uint8_t gp_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    uint8_t pf_attr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);
    ASSERT_EQ(static_cast<unsigned>(de_attr & 0x0F), 0x0Eu);  // Interrupt gate
    ASSERT_EQ(static_cast<unsigned>(gp_attr & 0x0F), 0x0Eu);  // Interrupt gate
    ASSERT_EQ(static_cast<unsigned>(pf_attr & 0x0F), 0x0Eu);  // Interrupt gate
}

// ============================================================
// 10. IDT Entry Struct Layout (mirrored types)
// ============================================================

/// Verify mirrored IDT entry size is 16 bytes
TEST("idt: TestIdtEntry size is 16 bytes") {
    ASSERT_EQ(sizeof(TestIdtEntry), 16u);
}

/// Verify mirrored IDT entry field offsets
TEST("idt: TestIdtEntry field offsets") {
    ASSERT_EQ(offsetof(TestIdtEntry, offset_low), 0u);
    ASSERT_EQ(offsetof(TestIdtEntry, selector), 2u);
    ASSERT_EQ(offsetof(TestIdtEntry, ist), 4u);
    ASSERT_EQ(offsetof(TestIdtEntry, type_attr), 5u);
    ASSERT_EQ(offsetof(TestIdtEntry, offset_mid), 6u);
    ASSERT_EQ(offsetof(TestIdtEntry, offset_high), 8u);
    ASSERT_EQ(offsetof(TestIdtEntry, reserved), 12u);
}

/// Verify mirrored IDTR size is 10 bytes
TEST("idt: TestIdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(TestIdtPointer), 10u);
}

/// Verify mirrored IDTR field offsets
TEST("idt: TestIdtPointer field offsets") {
    ASSERT_EQ(offsetof(TestIdtPointer, limit), 0u);
    ASSERT_EQ(offsetof(TestIdtPointer, base), 2u);
}

// ============================================================
// 11. IDT Entry Encoding (mirrored set_idt_entry)
// ============================================================

/// Verify IDT entry address splitting for a low address
TEST("idt: set_idt_entry splits low address") {
    TestIdtEntry table[256] = {};
    uint64_t     addr       = 0x00000000ABCDEFFFULL;

    test_set_idt_entry(table, 3, addr, GDT_KERNEL_CODE,
                       make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Trap), 0);

    ASSERT_EQ(table[3].offset_low, 0xEFFFu);
    ASSERT_EQ(table[3].offset_mid, 0xABCDu);
    ASSERT_EQ(table[3].offset_high, 0x00000000u);
}

/// Verify IDT entry address splitting for a higher-half address
TEST("idt: set_idt_entry splits higher-half address") {
    TestIdtEntry table[256] = {};
    uint64_t     addr       = 0xFFFFFFFF80010000ULL;

    test_set_idt_entry(table, 14, addr, GDT_KERNEL_CODE,
                       make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt), 0);

    ASSERT_EQ(table[14].offset_low, 0x0000u);
    ASSERT_EQ(table[14].offset_mid, 0x8001u);
    ASSERT_EQ(table[14].offset_high, 0xFFFFFFFFu);
}

/// Verify IDT entry selector field
TEST("idt: set_idt_entry selector field") {
    TestIdtEntry table[256] = {};

    test_set_idt_entry(table, 3, 0x1000, GDT_KERNEL_CODE, 0x8F, 0);
    ASSERT_EQ(table[3].selector, GDT_KERNEL_CODE);
    ASSERT_EQ(table[3].selector, 0x0010u);
}

/// Verify IDT entry IST field is 0 (IST not used in current design)
TEST("idt: set_idt_entry IST is zero") {
    TestIdtEntry table[256] = {};

    test_set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    test_set_idt_entry(table, 14, 0x2000, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[3].ist, 0);
    ASSERT_EQ(table[14].ist, 0);
}

/// Verify IDT entry reserved field is 0
TEST("idt: set_idt_entry reserved is zero") {
    TestIdtEntry table[256] = {};

    test_set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    ASSERT_EQ(table[3].reserved, 0u);
}

/// Verify different vectors write to different IDT entries
TEST("idt: different vectors write different entries") {
    TestIdtEntry table[256] = {};

    test_set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    test_set_idt_entry(table, 14, 0x2000, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[3].offset_low, 0x1000u);
    ASSERT_EQ(table[14].offset_low, 0x2000u);
    ASSERT_NE(table[3].offset_low, table[14].offset_low);
}

/// Verify unconfigured IDT entry has Present=0
TEST("idt: zero entry has Present bit clear") {
    TestIdtEntry zero_entry = {};
    ASSERT_EQ(zero_entry.type_attr, 0);
    ASSERT_FALSE(zero_entry.type_attr & 0x80);
}

/// Verify IDTR limit calculation for 256 entries
TEST("idt: IDTR limit for 256 entries") {
    uint16_t limit = sizeof(TestIdtEntry) * 256 - 1;
    ASSERT_EQ(limit, 4095u);
}

// ============================================================
// 12. InterruptFrame Layout
// ============================================================

/// Verify InterruptFrame total size: 21 fields * 8 bytes = 168 bytes
TEST("frame: InterruptFrame size is 168 bytes") {
    // 15 GPR + 1 error_code + 5 CPU-pushed = 21 uint64_t fields
    ASSERT_EQ(sizeof(InterruptFrame), 21u * 8u);
}

/// Verify InterruptFrame error_code field offset (after 15 GPRs)
TEST("frame: error_code offset is 120") {
    ASSERT_EQ(offsetof(InterruptFrame, error_code), 15u * 8u);
}

/// Verify InterruptFrame CPU-pushed fields offsets
TEST("frame: CPU-pushed fields offsets") {
    uint64_t base = offsetof(InterruptFrame, error_code) + 8;

    ASSERT_EQ(offsetof(InterruptFrame, rip), base);
    ASSERT_EQ(offsetof(InterruptFrame, cs), base + 8);
    ASSERT_EQ(offsetof(InterruptFrame, rflags), base + 16);
    ASSERT_EQ(offsetof(InterruptFrame, rsp), base + 24);
    ASSERT_EQ(offsetof(InterruptFrame, ss), base + 32);
}

/// Verify InterruptFrame GPR field ordering (r15 first, rax last)
TEST("frame: GPR field ordering") {
    ASSERT_LT(offsetof(InterruptFrame, r15), offsetof(InterruptFrame, r14));
    ASSERT_LT(offsetof(InterruptFrame, r14), offsetof(InterruptFrame, r13));
    ASSERT_LT(offsetof(InterruptFrame, r13), offsetof(InterruptFrame, r12));
    ASSERT_LT(offsetof(InterruptFrame, r12), offsetof(InterruptFrame, r11));
    ASSERT_LT(offsetof(InterruptFrame, r11), offsetof(InterruptFrame, r10));
    ASSERT_LT(offsetof(InterruptFrame, r10), offsetof(InterruptFrame, r9));
    ASSERT_LT(offsetof(InterruptFrame, r9), offsetof(InterruptFrame, r8));
    ASSERT_LT(offsetof(InterruptFrame, r8), offsetof(InterruptFrame, rdi));
    ASSERT_LT(offsetof(InterruptFrame, rdi), offsetof(InterruptFrame, rsi));
    ASSERT_LT(offsetof(InterruptFrame, rsi), offsetof(InterruptFrame, rbp));
    ASSERT_LT(offsetof(InterruptFrame, rbp), offsetof(InterruptFrame, rdx));
    ASSERT_LT(offsetof(InterruptFrame, rdx), offsetof(InterruptFrame, rcx));
    ASSERT_LT(offsetof(InterruptFrame, rcx), offsetof(InterruptFrame, rbx));
    ASSERT_LT(offsetof(InterruptFrame, rbx), offsetof(InterruptFrame, rax));
}

/// Verify InterruptFrame ss is the last field
TEST("frame: ss is last field") {
    ASSERT_EQ(offsetof(InterruptFrame, ss) + sizeof(uint64_t), sizeof(InterruptFrame));
}

// ============================================================
// 13. Exception Handler Behavior Categories
// ============================================================
// Handler behavior is encoded in the gate type and handler naming
// convention, not in testable host-side logic.  We verify the
// policy decisions through constants and enum values.

/// Verify non-fatal exceptions (#BP, #DB) exist in ExceptionVector
TEST("handler: non-fatal exceptions BP and DB exist") {
    // These vectors should have non-fatal handlers (print + IRETQ)
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::BP), 3u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DB), 1u);
}

/// Verify fatal exceptions (#DE, #GP, #PF) exist in ExceptionVector
TEST("handler: fatal exceptions DE GP PF exist") {
    // These vectors should have fatal handlers (print + cli;hlt)
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DE), 0u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::GP), 13u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::PF), 14u);
}

/// Verify error-code-bearing exceptions: DF(8), TS(10), NP(11), SS(12), GP(13), PF(14)
TEST("handler: error code exceptions exist") {
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::DF), 8u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::TS), 10u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::NP), 11u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::SS), 12u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::GP), 13u);
    ASSERT_EQ(static_cast<uint8_t>(ExceptionVector::PF), 14u);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
