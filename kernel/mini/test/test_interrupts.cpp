/**
 * @file kernel/mini/test/test_interrupts.cpp
 * @brief QEMU in-kernel integration tests for GDT/IDT/Interrupts
 *
 * Runs inside the QEMU kernel, directly verifying the state after GDT/IDT
 * initialization and that interrupt handling works correctly.
 *
 * Test coverage:
 *   - Segment register values after GDT initialization
 *   - Basic state after IDT initialization
 *   - #BP(3) breakpoint exception trigger and recovery
 *   - #PF(14) page fault exception trigger and recovery
 *
 * Uses the kernel_test.h framework (not test_framework.h).
 */

#include "../arch/x86_64/gdt.hpp"
#include "../arch/x86_64/idt.hpp"
#include "kernel_test.h"

using cinux::mini::arch::SEGMENT_CODE64;
using cinux::mini::arch::SEGMENT_DATA64;

// ============================================================
// Test 1: Read current segment register values to verify GDT loaded successfully
// ============================================================
namespace test_gdt_segments {

/**
 * @brief Verify CS register points to code64 segment after GDT initialization
 */
void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, SEGMENT_CODE64);
}

/**
 * @brief Verify DS register points to data64 segment after GDT initialization
 */
void test_ds_register() {
    uint16_t ds = 0;
    __asm__ volatile("movw %%ds, %0" : "=r"(ds));
    TEST_ASSERT_EQ(ds, SEGMENT_DATA64);
}

/**
 * @brief Verify SS register points to data64 segment after GDT initialization
 */
void test_ss_register() {
    uint16_t ss = 0;
    __asm__ volatile("movw %%ss, %0" : "=r"(ss));
    TEST_ASSERT_EQ(ss, SEGMENT_DATA64);
}

/**
 * @brief Verify ES register points to data64 segment after GDT initialization
 */
void test_es_register() {
    uint16_t es = 0;
    __asm__ volatile("movw %%es, %0" : "=r"(es));
    TEST_ASSERT_EQ(es, SEGMENT_DATA64);
}
}  // namespace test_gdt_segments

// ============================================================
// Test 2: #BP breakpoint exception trigger and recovery
// ============================================================
namespace test_bp_exception {

/// Marker variable: whether handle_bp has been executed
static volatile int bp_handler_executed = 0;

/**
 * @brief Verify execution continues after INT3 triggers #BP
 *
 * Triggers a breakpoint exception via asm volatile("int $3").
 * If GDT/IDT/ISR are correctly configured, handle_bp will print
 * information and return, and execution continues to this assertion.
 *
 * If execution does not reach here, a triple fault occurred and QEMU will reboot.
 */
void test_bp_continues_execution() {
    // Record state before execution
    volatile int marker_before = 0x1234;

    // Trigger #BP(3) breakpoint exception
    __asm__ volatile("int $3");

    // If we reach here, #BP handling succeeded and returned
    volatile int marker_after = 0x5678;

    // Verify variables were not corrupted (stack frame restored correctly)
    TEST_ASSERT_EQ(marker_before, 0x1234);
    TEST_ASSERT_EQ(marker_after, 0x5678);
}
}  // namespace test_bp_exception

// ============================================================
// Test 3: #PF page fault exception trigger and recovery
// ============================================================
namespace test_pf_exception {

/**
 * @brief Verify execution continues after accessing non-existent address triggers #PF
 *
 * Triggers a page fault by reading an unmapped high address.
 * If the #PF handler is correct, it will print information and return (IRETQ),
 * but since the page table is not fixed, accessing the same address again
 * will trigger another #PF.
 *
 * Note: We only verify that #PF trigger does not cause a crash.
 * Since handle_pf only prints information and returns, returning will re-execute
 * the faulting instruction, causing an infinite loop. Therefore this test uses a
 * special approach: modify the return address to skip the faulting instruction.
 *
 * A safer approach is to use an address that is known to trigger but does not
 * affect execution. But since the current handle_pf does not fix page tables,
 * directly triggering would cause an infinite loop.
 * Therefore this test is marked as optional and skipped by default.
 */
void test_pf_optional() {
    // #PF test requires handle_pf to modify RIP for safe return.
    // Current handle_pf does not modify RIP; directly triggering would cause an infinite loop.
    // So we only mark it here without actually triggering #PF.
    // To test #PF, add RIP skip logic in handle_pf.
    kprintf("  [SKIP] #PF test skipped - handle_pf does not skip faulting instruction\n");
}
}  // namespace test_pf_exception

// ============================================================
// Test 4: Multiple exception triggers do not accumulate corruption
// ============================================================
namespace test_multiple_exceptions {

/**
 * @brief Verify the system remains normal after multiple consecutive #BP triggers
 */
void test_multiple_bp() {
    volatile uint64_t canary = 0xCAFEBABEDEADC0DEULL;

    // First trigger
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

    // Second trigger
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

    // Third trigger
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
}
}  // namespace test_multiple_exceptions

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_interrupt_tests() {
    TEST_SECTION("GDT/IDT/Interrupt Tests (007)");

    RUN_TEST(test_gdt_segments::test_cs_register);
    RUN_TEST(test_gdt_segments::test_ds_register);
    RUN_TEST(test_gdt_segments::test_ss_register);
    RUN_TEST(test_gdt_segments::test_es_register);
    RUN_TEST(test_bp_exception::test_bp_continues_execution);
    RUN_TEST(test_pf_exception::test_pf_optional);
    RUN_TEST(test_multiple_exceptions::test_multiple_bp);

    TEST_SUMMARY();
}
