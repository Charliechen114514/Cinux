/**
 * @file kernel/test/main_test.cpp
 * @brief Big kernel test entry point
 *
 * Replaces the production kernel_main with a test harness that initializes
 * GDT/IDT, runs the GDT/IDT test suite, and exits via QEMU isa-debug-exit.
 *
 * Exit codes:
 *   0 = all tests passed (QEMU exits with code 1 via isa-debug-exit)
 *   1 = some tests failed (QEMU exits with code 3 via isa-debug-exit)
 */

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"

#include "boot/boot_info.h"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

#include "big_kernel_test.h"

extern "C" {
void run_gdt_idt_tests();
void run_pic_pit_tests();
void run_video_tests();
void run_keyboard_tests();
void run_pmm_tests();
void run_vmm_tests();
}

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

extern "C" void kernel_main() {
    // Step 1: Initialise serial port for test output
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[TEST] Big Kernel Test Suite starting...\n");

    // Step 2: Initialise GDT (must come before IDT)
    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[TEST] GDT loaded.\n");

    // Step 3: Initialise IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[TEST] IDT loaded.\n");

    // Step 4: Run test suites
    run_gdt_idt_tests();

    // PIC/PIT/IRQ tests handle their own init (PIC, PIT, irq_init, STI/CLI)
    // so we run them after the basic GDT/IDT tests.
    run_pic_pit_tests();

    // Video tests use real VBE framebuffer (set up by bootloader)
    run_video_tests();

    // Keyboard tests use PS/2 controller (QEMU emulated)
    run_keyboard_tests();

    // PMM tests: initialise with real BootInfo, then run tests
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    cinux::mm::g_pmm.init(*boot_info);
    run_pmm_tests();

    // VMM tests: initialise VMM after PMM, then run tests
    cinux::mm::g_vmm.init();
    run_vmm_tests();

    // Step 5: Report and exit
    int exit_code = (test::get_total_failed() > 0) ? 1 : 0;

    if (exit_code != 0) {
        cinux::lib::kprintf("\n[TEST] TESTS FAILED (exit code %d)\n", exit_code);
    } else {
        cinux::lib::kprintf("\n[TEST] ALL TESTS PASSED (exit code %d)\n", exit_code);
    }

    // Exit via QEMU isa-debug-exit device (port 0xf4)
    __asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));

    // Fallback halt if isa-debug-exit is not available
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
