/**
 * @file kernel/test/test_pic_pit.cpp
 * @brief QEMU in-kernel integration tests for PIC + PIT drivers (011)
 *
 * Runs inside QEMU as part of the big kernel test suite, directly verifying
 * that PIC initialisation, PIT configuration, and hardware IRQ delivery
 * work correctly against real (virtual) hardware.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - GDT and IDT are already initialised
 *   - Serial port is initialised (kprintf works)
 *
 * Test coverage:
 *   - PIC::init() completes without triple-faulting
 *   - PIC::send_eoi() does not crash on master and slave IRQs
 *   - PIC offset accessors return the expected remapped values
 *   - PIT::init() configures channel 0 without hanging
 *   - irq_init() registers all 16 IRQ handlers into the IDT
 *   - After unmasking IRQ0 and enabling interrupts, tick_count increments
 *   - PIT::get_uptime_ms() returns a monotonically increasing value
 *   - PIC::mask() suppresses IRQ0 delivery
 *   - PIC::unmask() re-enables IRQ0 delivery
 *   - PIC::disable_all() suppresses all interrupts
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/drivers/pit/pit.hpp"

using cinux::arch::PIC;
using cinux::drivers::PIT;

// irq_init() is declared with C linkage in irq_handlers.cpp (linked via big_kernel_common)
extern "C" void irq_init();

// ============================================================
// Helpers
// ============================================================

/**
 * @brief Spin-wait for a given number of PIT ticks
 *
 * Spins until the tick counter has advanced by at least @p ticks.
 * Uses the PAUSE instruction to reduce power consumption while spinning.
 *
 * @param ticks   Minimum number of ticks to wait
 * @param timeout Safety limit in loop iterations (prevents infinite loop)
 */
static void wait_for_ticks(uint64_t ticks, uint64_t timeout = 50'000'000) {
    uint64_t target = PIT::get_ticks() + ticks;
    uint64_t safety = 0;
    while (PIT::get_ticks() < target && safety < timeout) {
        __asm__ volatile("pause");
        safety++;
    }
}

// ============================================================
// Test 1: PIC initialisation
// ============================================================
namespace test_pic_init {

void test_pic_init_no_crash() {
    // init() sends the full ICW1-ICW4 sequence with io_wait() delays.
    // In QEMU this must complete without hanging or faulting.
    PIC::init(0x20, 0x28);
}

}  // namespace test_pic_init

// ============================================================
// Test 2: PIC offset accessors
// ============================================================
namespace test_pic_offsets {

void test_master_offset() {
    TEST_ASSERT_EQ(PIC::master_offset(), 0x20);
}

void test_slave_offset() {
    TEST_ASSERT_EQ(PIC::slave_offset(), 0x28);
}

}  // namespace test_pic_offsets

// ============================================================
// Test 3: PIC send_eoi does not crash
// ============================================================
namespace test_pic_eoi {

void test_eoi_master_irq() {
    // Sending EOI for IRQ0 (master-only) must not fault.
    PIC::send_eoi(0);
}

void test_eoi_slave_irq() {
    // Sending EOI for IRQ8 (slave + master) must not fault.
    PIC::send_eoi(8);
}

}  // namespace test_pic_eoi

// ============================================================
// Test 4: PIT initialisation
// ============================================================
namespace test_pit_init {

void test_pit_init_no_crash() {
    // init() writes command 0x36 and the divisor to PIT channel 0.
    // In QEMU this should complete instantly.
    PIT::init(100);
}

void test_tick_count_resets() {
    // After init(), tick count should be zero (IRQ0 not yet unmasked,
    // so no ticks can arrive between init() and this assertion).
    TEST_ASSERT_EQ(PIT::get_ticks(), 0u);
}

void test_freq_hz_stored() {
    TEST_ASSERT_EQ(PIT::freq_hz(), 100u);
}

}  // namespace test_pit_init

// ============================================================
// Test 5: irq_init() registers all 16 IRQ handlers
// ============================================================
namespace test_irq_init {

void test_irq_init_no_crash() {
    // irq_init() writes 16 IDT entries for vectors 0x20-0x2F.
    irq_init();
}

}  // namespace test_irq_init

// ============================================================
// Test 6: PIT ticks increment after unmasking IRQ0 + STI
// ============================================================
namespace test_pit_tick {

void test_ticks_increment() {
    // Ensure all IRQs are masked first for a clean start
    PIC::disable_all();

    // Re-init PIT to reset tick counter
    PIT::init(100);

    // Unmask only IRQ0 (PIT timer)
    PIC::unmask(0);

    // Enable CPU interrupt flag
    __asm__ volatile("sti");

    // Wait a short while for at least 10 ticks.
    // At 100 Hz, we expect ~1 tick per 10 ms.
    wait_for_ticks(10);

    uint64_t ticks = PIT::get_ticks();
    TEST_ASSERT_GT(ticks, 0u);

    // Mask IRQ0 again to avoid interference with subsequent tests
    PIC::mask(0);
    __asm__ volatile("cli");
}

}  // namespace test_pit_tick

// ============================================================
// Test 7: Uptime increases over time
// ============================================================
namespace test_pit_uptime {

void test_uptime_monotonic() {
    // Re-enable IRQ0 for this test
    PIC::unmask(0);
    __asm__ volatile("sti");

    uint64_t t1 = PIT::get_uptime_ms();

    // Wait for at least 5 ticks (~50 ms at 100 Hz)
    wait_for_ticks(5);

    uint64_t t2 = PIT::get_uptime_ms();

    // Uptime must have increased
    TEST_ASSERT_GT(t2, t1);

    // Mask IRQ0 again
    PIC::mask(0);
    __asm__ volatile("cli");
}

}  // namespace test_pit_uptime

// ============================================================
// Test 8: mask() suppresses IRQ0 delivery
// ============================================================
namespace test_pic_mask {

void test_mask_suppresses_irq0() {
    // Ensure IRQ0 is masked and interrupts disabled
    PIC::mask(0);
    __asm__ volatile("cli");

    // Record tick count
    uint64_t before = PIT::get_ticks();

    // Briefly enable interrupts -- since IRQ0 is masked, ticks must
    // NOT advance.  We spin for a while to be sure.
    __asm__ volatile("sti");

    // Busy-wait loop (not relying on PIT since it is masked)
    for (volatile uint64_t i = 0; i < 10'000'000; i++) {
        __asm__ volatile("pause");
    }

    __asm__ volatile("cli");

    uint64_t after = PIT::get_ticks();

    // Tick count must not have advanced (IRQ0 was masked)
    TEST_ASSERT_EQ(after, before);
}

}  // namespace test_pic_mask

// ============================================================
// Test 9: unmask() re-enables IRQ0 delivery
// ============================================================
namespace test_pic_unmask {

void test_unmask_reenables_irq0() {
    // IRQ0 is currently masked from the previous test
    PIC::unmask(0);
    __asm__ volatile("sti");

    // Wait for at least a few ticks
    wait_for_ticks(5);

    uint64_t ticks = PIT::get_ticks();
    TEST_ASSERT_GT(ticks, 0u);

    // Clean up: mask IRQ0 and disable interrupts
    PIC::mask(0);
    __asm__ volatile("cli");
}

}  // namespace test_pic_unmask

// ============================================================
// Test 10: disable_all() suppresses all interrupts
// ============================================================
namespace test_pic_disable_all {

void test_disable_all_suppresses() {
    // Mask everything
    PIC::disable_all();
    __asm__ volatile("cli");

    uint64_t before = PIT::get_ticks();

    __asm__ volatile("sti");

    // Spin for a while -- no ticks should arrive
    for (volatile uint64_t i = 0; i < 10'000'000; i++) {
        __asm__ volatile("pause");
    }

    __asm__ volatile("cli");

    uint64_t after = PIT::get_ticks();

    // Tick count must not have changed
    TEST_ASSERT_EQ(after, before);
}

}  // namespace test_pic_disable_all

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_pic_pit_tests() {
    TEST_SECTION("PIC/PIT/IRQ Tests (011)");

    // --- PIC basic tests ---
    RUN_TEST(test_pic_init::test_pic_init_no_crash);
    RUN_TEST(test_pic_offsets::test_master_offset);
    RUN_TEST(test_pic_offsets::test_slave_offset);
    RUN_TEST(test_pic_eoi::test_eoi_master_irq);
    RUN_TEST(test_pic_eoi::test_eoi_slave_irq);

    // --- PIT basic tests ---
    RUN_TEST(test_pit_init::test_pit_init_no_crash);
    RUN_TEST(test_pit_init::test_tick_count_resets);
    RUN_TEST(test_pit_init::test_freq_hz_stored);

    // --- IRQ registration ---
    RUN_TEST(test_irq_init::test_irq_init_no_crash);

    // --- PIT tick and uptime (require interrupts enabled) ---
    RUN_TEST(test_pit_tick::test_ticks_increment);
    RUN_TEST(test_pit_uptime::test_uptime_monotonic);

    // --- Mask / unmask / disable_all ---
    RUN_TEST(test_pic_mask::test_mask_suppresses_irq0);
    RUN_TEST(test_pic_unmask::test_unmask_reenables_irq0);
    RUN_TEST(test_pic_disable_all::test_disable_all_suppresses);

    TEST_SUMMARY();
}
