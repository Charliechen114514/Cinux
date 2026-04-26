/**
 * @file kernel/test/test_gdt_idt.cpp
 * @brief QEMU in-kernel integration tests for big kernel GDT/IDT/Interrupts
 *
 * Runs inside QEMU, directly verifying the state after GDT/IDT
 * initialization and that interrupt handling works correctly.
 *
 * Test coverage:
 *   - Segment register values after GDT initialization
 *   - #BP(3) breakpoint exception trigger and recovery
 *   - Stack frame integrity across multiple exceptions
 *   - IDT gate type policy (trap gate for #BP/#DB)
 */

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"

using cinux::arch::GDT_KERNEL_CODE;
using cinux::arch::GDT_KERNEL_DATA;

// ============================================================
// Test 1: Segment registers after GDT initialization
// ============================================================

namespace test_gdt_segments {

void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, GDT_KERNEL_CODE);
}

void test_ds_register() {
    uint16_t ds = 0;
    __asm__ volatile("movw %%ds, %0" : "=r"(ds));
    TEST_ASSERT_EQ(ds, GDT_KERNEL_DATA);
}

void test_ss_register() {
    uint16_t ss = 0;
    __asm__ volatile("movw %%ss, %0" : "=r"(ss));
    TEST_ASSERT_EQ(ss, GDT_KERNEL_DATA);
}

void test_es_register() {
    uint16_t es = 0;
    __asm__ volatile("movw %%es, %0" : "=r"(es));
    TEST_ASSERT_EQ(es, GDT_KERNEL_DATA);
}

}  // namespace test_gdt_segments

// ============================================================
// Test 2: #BP breakpoint exception trigger and recovery
// ============================================================

namespace test_bp_exception {

void test_bp_continues_execution() {
    volatile int marker_before = 0x1234;

    __asm__ volatile("int $3");

    // If we reach here, #BP handler ran and IRETQ returned correctly
    volatile int marker_after = 0x5678;

    TEST_ASSERT_EQ(marker_before, 0x1234);
    TEST_ASSERT_EQ(marker_after, 0x5678);
}

}  // namespace test_bp_exception

// ============================================================
// Test 3: Multiple exceptions do not corrupt state
// ============================================================

namespace test_multiple_exceptions {

void test_multiple_bp() {
    volatile uint64_t canary = 0xCAFEBABEDEADC0DEULL;

    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
}

}  // namespace test_multiple_exceptions

// ============================================================
// Test 4: IDT gate type policy verification
// ============================================================

namespace test_idt_policy {

/// Verify make_idt_attr produces correct type_attr for #BP (user trap gate)
void test_bp_gate_is_user_trap() {
    uint8_t attr =
        cinux::arch::make_idt_attr(cinux::arch::IDTPrivilege::User, cinux::arch::IDTGateType::Trap);
    // Present(0x80) | DPL3(0x60) | Trap(0x0F) = 0xEF
    TEST_ASSERT_EQ(attr, 0xEFu);
}

/// Verify make_idt_attr produces correct type_attr for fatal exceptions
void test_fatal_gate_is_kernel_interrupt() {
    uint8_t attr = cinux::arch::make_idt_attr(cinux::arch::IDTPrivilege::Kernel,
                                              cinux::arch::IDTGateType::Interrupt);
    // Present(0x80) | DPL0(0x00) | Interrupt(0x0E) = 0x8E
    TEST_ASSERT_EQ(attr, 0x8Eu);
}

}  // namespace test_idt_policy

// ============================================================
// Test Entry Point
// ============================================================

extern "C" void run_gdt_idt_tests() {
    TEST_SECTION("Big Kernel GDT/IDT/Interrupt Tests (010)");

    RUN_TEST(test_gdt_segments::test_cs_register);
    RUN_TEST(test_gdt_segments::test_ds_register);
    RUN_TEST(test_gdt_segments::test_ss_register);
    RUN_TEST(test_gdt_segments::test_es_register);
    RUN_TEST(test_bp_exception::test_bp_continues_execution);
    RUN_TEST(test_multiple_exceptions::test_multiple_bp);
    RUN_TEST(test_idt_policy::test_bp_gate_is_user_trap);
    RUN_TEST(test_idt_policy::test_fatal_gate_is_kernel_interrupt);

    TEST_SUMMARY();
}
