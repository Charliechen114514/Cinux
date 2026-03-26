/* ==============================================================
 * Cinux Mini Kernel - Test Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include "../lib/kprintf.h"
#include "boot_info.h"
#include "kernel_test.h"

using cinux::mini::lib::kprintf;
using cinux::mini::lib::kdebugf;

extern "C" {
extern uint64_t __boot_info_ptr;
void			run_cpp_tests();   // C++ runtime tests from test_cpp_basic.cpp
void			run_pmm_tests();   // PMM tests from test_pmm.cpp (006)
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
	// PMM Tests (006)
	// ============================================================
	run_pmm_tests();

	// ============================================================
	// Test Complete - Shutdown
	// ============================================================
	kprintf("\n=== All tests completed ===\n");

	// 计算退出码：0=成功，非0=失败
	int exit_code = (test::get_total_failed() > 0) ? 1 : 0;
	if (exit_code != 0) {
		kprintf("=== TESTS FAILED (exit code %d) ===\n", exit_code);
	} else {
		kprintf("=== ALL TESTS PASSED (exit code %d) ===\n", exit_code);
	}

	// 使用 QEMU 的 isa-debug-exit 设备安全退出
	// 向端口 0xf4 写入双字，高字节是退出码
	__asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));

	// 如果 QEMU 没有退出（比如在没有 isa-debug-exit 的环境下），则停机
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
