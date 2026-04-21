/* ==============================================================
 * Cinux Mini Kernel - Test Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include "../arch/x86_64/gdt.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../big_kernel_loader.hpp"
#include "../lib/kprintf.h"
#include "boot_info.h"
#include "kernel_test.h"

using cinux::mini::lib::kprintf;
using cinux::mini::lib::kdebugf;

extern "C" {
extern uint64_t __boot_info_ptr;
void			run_cpp_tests();		 // C++ runtime tests from test_cpp_basic.cpp
void			run_pmm_tests();		 // PMM tests from test_pmm.cpp (006)
void			run_interrupt_tests();	 // GDT/IDT/interrupt tests from test_interrupts.cpp (007)
void			run_ata_tests();		 // ATA PIO tests from test_ata.cpp (008)
void			run_elf_loader_tests();	 // ELF loader tests from test_elf_loader.cpp (008)
uint64_t
run_big_kernel_load_tests();		 // Big kernel loading tests from test_big_kernel_load.cpp (009)
void run_stress_big_kernel_tests();	 // Stress test: large kernel load
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
	BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
	(void)boot_info_addr;
	(void)boot_info;

	// ============================================================
	// kprintf/kdebugf Tests
	// ============================================================
	kprintf("=== kprintf Test ===\n");
	kprintf("String: %s\n", "Hello, Cinux!");
	kprintf("Char: %c\n", 'X');
	kprintf("Decimal: %d\n", -12345);
	kprintf("Unsigned: %u\n", 42);
	kprintf("Hex lower: %x\n", 0xDEADBEEF);
	kprintf("Hex upper: %X\n", 0xDEADBEEF);
	kprintf("Pointer: %p\n", 0xFFFFFFFF80000000ULL);
	kprintf("Binary: %b\n", 0b101010);
	kprintf("Width test: [%4d]\n", 7);
	kprintf("Zero pad: [%04d]\n", 7);
	kprintf("Null string: %s\n", nullptr);
	kprintf("Percent: %%\n");

	kdebugf("=== kdebugf Test ===\n");
	kdebugf("Value: %d, Hex: %x\n", -42, 0xDEADBEEF);
	kdebugf("String: %s, Pointer: %p\n", "Debug", 0xFFFFFFFF80000000ULL);

	// ============================================================
	// C++ Runtime Tests
	// ============================================================
	run_cpp_tests();

	// ============================================================
	// Initialize GDT & IDT (required for interrupt tests)
	// ============================================================
	kprintf("[INIT] Setting up GDT...\n");
	cinux::mini::arch::gdt_init();
	kprintf("[INIT] GDT loaded successfully.\n");

	kprintf("[INIT] Setting up IDT...\n");
	cinux::mini::arch::idt_init();
	kprintf("[INIT] IDT loaded successfully.\n");

	// ============================================================
	// GDT/IDT/Interrupt Tests (007)
	// ============================================================
	run_interrupt_tests();

	// ============================================================
	// PMM Tests (006)
	// ============================================================
	run_pmm_tests();

	// ============================================================
	// ATA PIO Tests (008)
	// ============================================================
	run_ata_tests();

	// ============================================================
	// ELF Loader Tests (008)
	// ============================================================
	run_elf_loader_tests();

	// ============================================================
	// Big Kernel Loading Tests (009)
	// ============================================================
	uint64_t big_kernel_entry = run_big_kernel_load_tests();

	// Note: stress tests (run_stress_big_kernel_tests) require a special
	// disk image with a large synthetic ELF. They are NOT run as part of
	// the standard test suite. Use `make run-stress-test` to run them.

	// ============================================================
	// Test Complete
	// ============================================================
	kprintf("\n=== Mini kernel tests completed ===\n");

	// Calculate exit code: 0=success, non-0=failure
	int exit_code = (test::get_total_failed() > 0) ? 1 : 0;
	if (exit_code != 0) {
		kprintf("=== MINI KERNEL TESTS FAILED (exit code %d) ===\n", exit_code);
		__asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));
		while (1) {
			__asm__ volatile("cli; hlt");
		}
	}

	kprintf("=== MINI KERNEL TESTS PASSED ===\n");

	// Big kernel test was loaded into memory by run_big_kernel_load_tests().
	// Only jump if the entry point has a real kernel (cli + mov rsp, imm64).
	// The stress-test synthetic ELF only has cli followed by data patterns.
	if (big_kernel_entry == 0) {
		kprintf("\n=== No big kernel loaded, exiting ===\n");
		__asm__ volatile("outl %0, $0xf4" : : "a"(0));
		while (1)
			__asm__ volatile("cli; hlt");
	}

	// Real kernel _start: cli (0xFA) then mov rsp, imm (two valid encodings):
	//   REX.W MOV r/m64, imm32  → 48 C7 C4 (sign-extended)
	//   REX.W MOV r64, imm64    → 48 BC    (full 64-bit)
	auto* code			 = reinterpret_cast<const uint8_t*>(big_kernel_entry);
	bool  is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
			                   (code[2] == 0xC7 || code[2] == 0xBC);
	if (!is_real_kernel) {
		kprintf("\n=== Loaded ELF is not a real kernel, exiting ===\n");
		__asm__ volatile("outl %0, $0xf4" : : "a"(0));
		while (1)
			__asm__ volatile("cli; hlt");
	}

	kprintf("\n=== Launching big kernel test at 0x%p ===\n",
			reinterpret_cast<void*>(big_kernel_entry));
	auto big_entry = reinterpret_cast<void (*)()>(big_kernel_entry);
	big_entry();
	// Should not reach here — big kernel test exits via isa-debug-exit
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
