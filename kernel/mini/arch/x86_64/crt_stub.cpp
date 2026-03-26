/* ==============================================================
 * Cinux Mini Kernel - C++ Runtime Support
 * ==============================================================
 *
 * Minimal C++ runtime stub functions required for freestanding
 * kernel environment. These functions are normally provided by
 * libstdc++/libc++, but we need to provide our own since we're
 * building with -nostdlib and -ffreestanding.
 */

#include <stdint.h>

extern "C" {

// ============================================================
// Pure Virtual Function Call Handler
// ============================================================
// Called when a pure virtual function is called (should never happen)
[[noreturn]] void __cxa_pure_virtual() {
	// Infinite halt - pure virtual call is a programming error
	while (1) {
		__asm__ volatile(
			"cli; \
            hlt");
	}
}

// ============================================================
// Stack Smashing Protector Failure Handler
// ============================================================
// Called when stack protection detects corruption (with -fstack-protector)
[[noreturn]] void __stack_chk_fail() {
	// Infinite halt - stack corruption detected
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}

// ============================================================
// Atexit Handler (Minimal Implementation)
// ============================================================
// We don't support process termination, so this is a no-op
int __cxa_atexit(void (*)(void*), void*, void*) {
	return 0;  // Success (but we don't actually register anything)
}

// ============================================================
// Global Constructors Initialization
// ============================================================
// Called from boot.S to run all global constructors
// These are placed in the .init_array section by the compiler

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

// Helper to convert higher-half address to identity-mapped address
static inline void* hh_to_identity(void* hh_addr) {
	uint64_t addr = (uint64_t)hh_addr;
	// Strip higher-half base and add identity base
	return (void*)(addr - 0xFFFFFFFF80000000 + 0x20000);
}

void _init_global_ctors() {
	// __init_array_start/end are higher-half addresses
	// No conversion needed - we're running in higher-half now!
	void (**start)() = __init_array_start;
	void (**end)() = __init_array_end;

	for (void (**func)() = start; func != end; func++) {
		void (*ctor)() = *func;
		if (ctor != nullptr) {
			ctor();
		}
	}
}

}  // extern "C"

// ============================================================
// Operator new/delete (Minimal Implementation)
// ============================================================
// We don't support dynamic memory allocation yet, so these are stubs
// that halt if called. This prevents linker errors when using classes
// with virtual destructors.
// NOTE: These must be outside extern "C" as they require C++ linkage.

void operator delete(void* ptr) noexcept {
	// Halt - delete not supported
	(void)ptr;
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}

void operator delete(void* ptr, unsigned long size) noexcept {
	// Halt - sized delete not supported
	(void)ptr;
	(void)size;
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}

void* operator new(unsigned long size) {
	// Halt - new not supported
	(void)size;
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}

void* operator new[](unsigned long size) {
	// Halt - array new not supported
	(void)size;
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
