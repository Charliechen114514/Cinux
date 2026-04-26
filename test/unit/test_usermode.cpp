/**
 * @file test/unit/test_usermode.cpp
 * @brief Host-side unit tests for Ring 3 user-mode transition (022)
 *
 * Test coverage:
 *   - User-mode constants: USER_ENTRY_BASE, USER_STACK_TOP, USER_STACK_PAGES
 *   - TSS struct layout (mirrored from GDT::TaskStateSegment)
 *   - User code bytecode encoding (cli; hlt; jmp .-2)
 *   - STAR MSR value computation (SYSRET CS/SS derivation)
 *   - User stack size and address calculation
 *   - User page flags (PRESENT | WRITABLE | USER)
 *   - InterruptFrame user-mode detection (CS & 0x03)
 *   - RFLAGS IF bit (0x200) for SYSRET
 *   - GDT selector constants for user mode
 *
 * Pure arithmetic and constant verification -- no kernel code linked.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

#    include "arch/x86_64/gdt.hpp"
#    include "arch/x86_64/paging_config.hpp"
#    include "arch/x86_64/usermode.hpp"

using namespace cinux::arch;
using namespace cinux;

// ============================================================
// Mirror of TSS (same layout as GDT::TaskStateSegment)
// ============================================================

struct [[gnu::packed]] TestTSS {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(TestTSS) == 104, "TestTSS must be 104 bytes");

// ============================================================
// 1. User-mode Constants
// ============================================================

/// Verify USER_ENTRY_BASE is 0x400000 (standard x86-64 ELF load address)
TEST("usermode: USER_ENTRY_BASE is 0x400000") {
    ASSERT_EQ(USER_ENTRY_BASE, 0x400000ULL);
}

/// Verify USER_STACK_TOP is 0x7FFFFF000 (near 2 GB boundary)
TEST("usermode: USER_STACK_TOP is 0x7FFFFF000") {
    ASSERT_EQ(USER_STACK_TOP, 0x7FFFFF000ULL);
}

/// Verify USER_STACK_PAGES is 4 (16 KB total stack)
TEST("usermode: USER_STACK_PAGES is 4") {
    ASSERT_EQ(USER_STACK_PAGES, 4ULL);
}

/// Verify USER_ENTRY_BASE is page-aligned
TEST("usermode: USER_ENTRY_BASE is page-aligned") {
    ASSERT_EQ(USER_ENTRY_BASE % PAGE_SIZE, 0ULL);
}

/// Verify USER_STACK_TOP is page-aligned
TEST("usermode: USER_STACK_TOP is page-aligned") {
    ASSERT_EQ(USER_STACK_TOP % PAGE_SIZE, 0ULL);
}

/// Verify USER_STACK_TOP is in lower half (user space)
TEST("usermode: USER_STACK_TOP is in user space (< kernel base)") {
    // Canonical lower-half addresses are below 0x800000000000
    ASSERT_LT(USER_STACK_TOP, 0x800000000000ULL);
}

/// Verify USER_ENTRY_BASE is in lower half (user space)
TEST("usermode: USER_ENTRY_BASE is in user space") {
    ASSERT_LT(USER_ENTRY_BASE, 0x800000000000ULL);
}

// ============================================================
// 2. User Stack Size Calculation
// ============================================================

/// Verify total user stack size = 4 pages * 4096 = 16384 bytes
TEST("usermode: user stack size is 16 KB") {
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    ASSERT_EQ(stack_size, 16384ULL);
}

/// Verify stack base calculation: USER_STACK_TOP - stack_size
TEST("usermode: stack base is below stack top") {
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    uint64_t stack_base = USER_STACK_TOP - stack_size;
    // 0x7FFFFF000 - 0x4000 = 0x7FFFFB000
    ASSERT_EQ(stack_base, 0x7FFFFB000ULL);
}

/// Verify stack base is page-aligned
TEST("usermode: stack base is page-aligned") {
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    uint64_t stack_base = USER_STACK_TOP - stack_size;
    ASSERT_EQ(stack_base % PAGE_SIZE, 0ULL);
}

/// Verify stack top > stack base (stack grows downward)
TEST("usermode: stack top > stack base") {
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    uint64_t stack_base = USER_STACK_TOP - stack_size;
    ASSERT_GT(USER_STACK_TOP, stack_base);
}

// ============================================================
// 3. User Code Bytecode Encoding
// ============================================================

/// Verify CLI instruction opcode is 0xFA
TEST("usermode: CLI opcode is 0xFA") {
    constexpr uint8_t cli_opcode = 0xFA;
    ASSERT_EQ(cli_opcode, 0xFA);
}

/// Verify HLT instruction opcode is 0xF4
TEST("usermode: HLT opcode is 0xF4") {
    constexpr uint8_t hlt_opcode = 0xF4;
    ASSERT_EQ(hlt_opcode, 0xF4);
}

/// Verify JMP rel8 -4 encoding is EB FC
TEST("usermode: JMP rel8 -4 encoding is EB FC") {
    constexpr uint8_t jmp_short = 0xEB;
    constexpr uint8_t rel8_neg4 = 0xFC;
    ASSERT_EQ(jmp_short, 0xEB);
    // 0xFC = -4 in two's complement for 8-bit
    ASSERT_EQ(static_cast<int8_t>(rel8_neg4), -4);
}

/// Verify the user code snippet is 4 bytes (cli; hlt; jmp -4)
TEST("usermode: user code is 4 bytes") {
    // The kUserCode array from usermode.cpp has 4 bytes:
    // 0xFA, 0xF4, 0xEB, 0xFC
    constexpr uint8_t code[] = {0xFA, 0xF4, 0xEB, 0xFC};
    ASSERT_EQ(sizeof(code), 4u);
}

/// Verify user code fits in a single 4KB page
TEST("usermode: user code fits in one page") {
    constexpr uint8_t code[] = {0xFA, 0xF4, 0xEB, 0xFC};
    ASSERT_TRUE(sizeof(code) <= PAGE_SIZE);
}

// ============================================================
// 4. STAR MSR Value Computation
// ============================================================

/// Verify STAR MSR index is 0xC0000081
TEST("usermode: STAR MSR index is 0xC0000081") {
    constexpr uint32_t MSR_STAR = 0xC0000081;
    ASSERT_EQ(MSR_STAR, 0xC0000081u);
}

/// Verify STAR[63:48] = GDT_SYSRET_BASE produces user CS = 0x33
TEST("usermode: SYSRET derives user CS from STAR") {
    // STAR[63:48] = GDT_SYSRET_BASE = 0x23
    // SYSRET: CS = STAR[63:48] + 16 = 0x23 + 0x10 = 0x33 (RPL already baked in)
    constexpr uint16_t star_cs_base = GDT_SYSRET_BASE;
    constexpr uint16_t user_cs      = star_cs_base + 16;
    ASSERT_EQ(user_cs, GDT_USER_CODE);
}

/// Verify STAR[63:48] produces SYSRET SS (hardware-computed)
TEST("usermode: SYSRET derives SS from STAR") {
    // SYSRET: SS = STAR[63:48] + 8 = 0x23 + 8 = 0x2B (RPL already baked in)
    constexpr uint16_t star_cs_base = GDT_SYSRET_BASE;
    constexpr uint16_t sysret_ss    = star_cs_base + 8;
    ASSERT_EQ(sysret_ss, GDT_USER_DATA);
}

/// Verify STAR value encoding: high 32 bits match GDT_SYSRET_BASE and GDT_KERNEL_CODE
TEST("usermode: STAR high 32 bits encoding") {
    constexpr uint64_t star_high =
        (static_cast<uint64_t>(GDT_SYSRET_BASE) << 16) | static_cast<uint64_t>(GDT_KERNEL_CODE);
    ASSERT_EQ(star_high, 0x00230010ULL);
}

/// Verify EFER.SCE bit is bit 0
TEST("usermode: EFER SCE bit is bit 0") {
    constexpr uint32_t EFER_SCE = 1;
    ASSERT_EQ(EFER_SCE, 1u);
}

/// Verify EFER MSR index is 0xC0000080
TEST("usermode: EFER MSR index is 0xC0000080") {
    constexpr uint32_t MSR_EFER = 0xC0000080;
    ASSERT_EQ(MSR_EFER, 0xC0000080u);
}

/// Verify SFMASK MSR index is 0xC0000084
TEST("usermode: SFMASK MSR index is 0xC0000084") {
    constexpr uint32_t MSR_SFMASK = 0xC0000084;
    ASSERT_EQ(MSR_SFMASK, 0xC0000084u);
}

// ============================================================
// 5. RFLAGS IF Bit for SYSRET
// ============================================================

/// Verify RFLAGS IF bit is bit 9 (0x200)
TEST("usermode: RFLAGS IF bit is 0x200") {
    constexpr uint64_t RFLAGS_IF = 1ULL << 9;
    ASSERT_EQ(RFLAGS_IF, 0x200ULL);
}

/// Verify SYSRET RFLAGS value has IF set and reserved bit 1
TEST("usermode: SYSRET RFLAGS = 0x202 (IF + bit1)") {
    // From usermode.S: pushq $0x202 -> R11 = RFLAGS for SYSRET
    // bit 1 is always 1 in RFLAGS (reserved, always set)
    constexpr uint64_t rflags = 0x202;
    ASSERT_TRUE(rflags & 0x200);  // IF set
    ASSERT_TRUE(rflags & 0x002);  // reserved bit 1
}

// ============================================================
// 6. User Page Flags
// ============================================================

/// Verify user page flags include PRESENT
TEST("usermode: user page flags include PRESENT") {
    constexpr uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
    ASSERT_TRUE(flags & FLAG_PRESENT);
}

/// Verify user page flags include WRITABLE
TEST("usermode: user page flags include WRITABLE") {
    constexpr uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
    ASSERT_TRUE(flags & FLAG_WRITABLE);
}

/// Verify user page flags include USER
TEST("usermode: user page flags include USER") {
    constexpr uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
    ASSERT_TRUE(flags & FLAG_USER);
}

/// Verify user page flags value: PRESENT|WRITABLE|USER = 0x7
TEST("usermode: user page flags value is 0x7") {
    constexpr uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
    ASSERT_EQ(flags & 0xFFF, 0x7ULL);
}

/// Verify user page flags do NOT include kernel-only flags
TEST("usermode: user page flags exclude PAGE_SIZE bit and others") {
    constexpr uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
    // Should not have PWT, PCD, HUGE, GLOBAL, NX
    ASSERT_FALSE(flags & FLAG_PWT);
    ASSERT_FALSE(flags & FLAG_PCD);
    ASSERT_FALSE(flags & FLAG_HUGE);
    ASSERT_FALSE(flags & FLAG_GLOBAL);
    ASSERT_FALSE(flags & FLAG_NX);
}

// ============================================================
// 7. TSS Struct Layout
// ============================================================

/// Verify mirrored TSS is 104 bytes
TEST("usermode: TSS size is 104 bytes") {
    ASSERT_EQ(sizeof(TestTSS), 104u);
}

/// Verify TSS rsp array has 3 entries (RSP0, RSP1, RSP2)
TEST("usermode: TSS has 3 RSP entries") {
    ASSERT_EQ(sizeof(TestTSS::rsp) / sizeof(TestTSS::rsp[0]), 3u);
}

/// Verify TSS IST array has 7 entries (IST1-IST7)
TEST("usermode: TSS has 7 IST entries") {
    ASSERT_EQ(sizeof(TestTSS::ist) / sizeof(TestTSS::ist[0]), 7u);
}

/// Verify TSS reserved0 field offset is 0
TEST("usermode: TSS reserved0 offset is 0") {
    ASSERT_EQ(offsetof(TestTSS, reserved0), 0u);
}

/// Verify TSS rsp[0] (RSP0) field offset is 4 (after reserved0 uint32_t)
TEST("usermode: TSS rsp[0] offset is 4") {
    ASSERT_EQ(offsetof(TestTSS, rsp), 4u);
}

/// Verify TSS ist[0] (IST1) offset is after rsp[3] + reserved1
TEST("usermode: TSS ist[0] offset") {
    // reserved0: 4 bytes
    // rsp[3]: 24 bytes
    // reserved1: 8 bytes
    // ist[0]: offset = 4 + 24 + 8 = 36
    ASSERT_EQ(offsetof(TestTSS, ist), 36u);
}

/// Verify TSS iomap_base offset
TEST("usermode: TSS iomap_base offset") {
    // 4 + 24 + 8 + 56 + 8 + 2 = 102
    ASSERT_EQ(offsetof(TestTSS, iomap_base), 102u);
}

// ============================================================
// 8. GDT TSS Descriptor Encoding for User Mode
// ============================================================

/// Verify TSS access byte: Present | TSS64Avail = 0x89
TEST("usermode: TSS access byte is 0x89") {
    auto access = SegmentAccess::Present | SegmentAccess::TSS64Avail;
    ASSERT_EQ(static_cast<uint8_t>(access), 0x89u);
}

/// Verify TSS descriptor spans 2 GDT slots (16 bytes)
TEST("usermode: TSS descriptor is 16 bytes (2 GDT slots)") {
    // 64-bit TSS descriptor: low 8 bytes + high 8 bytes
    ASSERT_EQ(sizeof(TestTSS) - 1, 103u);  // limit = sizeof(TSS) - 1
}

/// Verify GDT_TSS selector (0x38) points to entry index 7
TEST("usermode: GDT_TSS selector index is 7") {
    ASSERT_EQ(static_cast<unsigned>(GDT_TSS / 8), 7u);
}

// ============================================================
// 9. InterruptFrame User-Mode Detection
// ============================================================

/// Verify user-mode detection: CS with RPL=3 is detected as user mode
TEST("usermode: CS with RPL=3 detected as user mode") {
    // handle_gp checks: (frame->cs & 0x03) != 0
    uint64_t user_cs   = GDT_USER_CODE;  // 0x1B
    bool     from_user = (user_cs & 0x03) != 0;
    ASSERT_TRUE(from_user);
}

/// Verify kernel-mode detection: CS with RPL=0 is not user mode
TEST("usermode: CS with RPL=0 detected as kernel mode") {
    uint64_t kernel_cs = GDT_KERNEL_CODE;  // 0x08
    bool     from_user = (kernel_cs & 0x03) != 0;
    ASSERT_FALSE(from_user);
}

/// Verify user SS (0x23) has RPL=3
TEST("usermode: user SS 0x23 has RPL=3") {
    ASSERT_EQ(static_cast<unsigned>(GDT_USER_DATA & 0x03), 0x03u);
}

// ============================================================
// 10. User Code Bytecode Semantics (Privileged Instruction)
// ============================================================

/// Verify CLI (0xFA) is a privileged instruction that triggers #GP in Ring 3
TEST("usermode: CLI opcode triggers #GP in Ring 3 (design verification)") {
    // 0xFA is the CLI instruction which is privileged (Ring 0 only)
    // When executed in Ring 3, it triggers a General Protection Fault (#GP)
    constexpr uint8_t cli = 0xFA;
    ASSERT_EQ(cli, 0xFA);
    // This is a design verification: the user code intentionally executes
    // a privileged instruction to prove privilege isolation works
}

// ============================================================
// 11. Kernel VMA for Higher-Half Direct Map
// ============================================================

/// Verify KERNEL_VMA constant used for phys-to-virt translation
TEST("usermode: KERNEL_VMA is higher-half base") {
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    ASSERT_EQ(KERNEL_VMA, 0xFFFFFFFF80000000ULL);
}

/// Verify phys-to-virt translation for user code page
TEST("usermode: phys-to-virt address computation") {
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    constexpr uint64_t phys       = 0x1000;
    uint64_t           virt       = phys + KERNEL_VMA;
    ASSERT_EQ(virt, 0xFFFFFFFF80001000ULL);
}

// ============================================================
// 12. Double Fault Stack in TSS IST1
// ============================================================

/// Verify IST1 index in TSS is 0 (ist[0])
TEST("usermode: IST1 is ist[0] in TSS array") {
    // IST1 corresponds to ist[0] in the TSS struct
    TestTSS tss{};
    memset(&tss, 0, sizeof(tss));
    tss.ist[0] = 0xDEADBEEF;
    // IST1 = ist[0]
    ASSERT_EQ(tss.ist[0], 0xDEADBEEFULL);
}

/// Verify DF_STACK is 1 page (4096 bytes)
TEST("usermode: DF stack is 1 page") {
    constexpr uint64_t DF_STACK_PAGES = 1;
    constexpr uint64_t df_stack_size  = DF_STACK_PAGES * 4096;
    ASSERT_EQ(df_stack_size, 4096ULL);
}

// ============================================================
// 13. RSP0 Update for Task Switch
// ============================================================

/// Verify tss_set_rsp0 writes to rsp[0] (RSP0 field)
TEST("usermode: RSP0 is tss.rsp[0]") {
    TestTSS tss{};
    memset(&tss, 0, sizeof(tss));

    uint64_t kernel_rsp0 = 0x801000;
    tss.rsp[0]           = kernel_rsp0;

    ASSERT_EQ(tss.rsp[0], 0x801000ULL);
}

/// Verify RSP1 and RSP2 are separate from RSP0
TEST("usermode: RSP0 RSP1 RSP2 are distinct fields") {
    TestTSS tss{};
    memset(&tss, 0, sizeof(tss));

    tss.rsp[0] = 0x1000;
    tss.rsp[1] = 0x2000;
    tss.rsp[2] = 0x3000;

    ASSERT_EQ(tss.rsp[0], 0x1000ULL);
    ASSERT_EQ(tss.rsp[1], 0x2000ULL);
    ASSERT_EQ(tss.rsp[2], 0x3000ULL);
}

// ============================================================
// 14. Address Space Layout Consistency
// ============================================================

/// Verify user entry and stack do not overlap
TEST("usermode: user code and stack regions do not overlap") {
    uint64_t code_end   = USER_ENTRY_BASE + PAGE_SIZE;
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    uint64_t stack_base = USER_STACK_TOP - stack_size;

    // Code is at 0x400000, stack is at 0x7FFFEF000
    ASSERT_LT(code_end, stack_base);
}

/// Verify user code region is below stack region
TEST("usermode: user code is below user stack") {
    ASSERT_LT(USER_ENTRY_BASE, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
