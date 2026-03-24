/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

// Include BootInfo definition from bootloader (shared header)
#include "../../boot/boot_info.h"

// Forward declarations (provided by boot.S)
extern "C" {
extern uint64_t __boot_info_ptr;
}

// Simple inline function for debugcon output
static void debugcon_putc(char c) {
	__asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

// ============================================================
// Test 1: Simple Class with Constructor/Destructor
// ============================================================
class SimpleClass {
private:
	int	 value;
	char marker;

public:
	SimpleClass(int v) : value(v), marker('S') {
		// Constructor: print 'C' + value digit
		debugcon_putc('C');
		debugcon_putc('0' + v);
	}

	~SimpleClass() {
		// Destructor: print 'D' + value digit
		debugcon_putc('D');
		debugcon_putc('0' + value);
	}

	int	 getValue() const { return value; }
	char getMarker() const { return marker; }
};

// ============================================================
// Test 2: Virtual Functions (vtable test)
// ============================================================
class Base {
public:
	virtual char getName() = 0;	 // Pure virtual
	virtual int	 compute() = 0;
	virtual ~Base() {}
};

class Derived : public Base {
private:
	int multiplier;

public:
	Derived(int m) : multiplier(m) {
		debugcon_putc('V');	 // 'V' for Virtual
	}

	virtual char getName() override {
		return 'D';	 // 'D' for Derived
	}

	virtual int compute() override { return multiplier * 2; }

	virtual ~Derived() override {
		debugcon_putc('d');	 // lowercase 'd' for Derived destructor
	}
};

// ============================================================
// Test 3: Global Object (global constructor test)
// ============================================================
static int global_construction_count = 0;

class GlobalCounter {
public:
	GlobalCounter() {
		global_construction_count = 42;	 // Magic value to verify ctor ran
		debugcon_putc('G');				 // 'G' for Global
	}

	int getCount() const { return global_construction_count; }
};

// Global object - constructor should be called by _init_global_ctors
GlobalCounter global_counter;

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
	// Use the global __boot_info_ptr (now in .data section, no corruption)
	BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
	(void)boot_info_addr;  // Suppress unused warning

	// Start marker: "===CPP"
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('C');
	debugcon_putc('P');
	debugcon_putc('P');

	// ============================================================
	// Test 1: Simple Class (stack allocation)
	// ============================================================
	// Expected output: C1 (constructor for value=1)
	SimpleClass obj1(1);

	// Verify object state
	if (obj1.getValue() == 1 && obj1.getMarker() == 'S') {
		debugcon_putc('1');  // '1' = SimpleClass test passed
	}

	// ============================================================
	// Test 2: Virtual Functions
	// ============================================================
	// Expected output: V (Derived constructor)
	Derived derived(5);
	Base* base = &derived;

	// Test vtable dispatch
	if (base->getName() == 'D' && base->compute() == 10) {
		debugcon_putc('2');  // '2' = Virtual function test passed
	}

	// ============================================================
	// Test 3: Global Object (already constructed by _init_global_ctors)
	// ============================================================
	if (global_counter.getCount() == 42) {
		debugcon_putc('3');  // '3' = Global constructor test passed
	}

	// ============================================================
	// Verify BootInfo
	// ============================================================
	if (boot_info->entry_point == 0xFFFFFFFF80020000 && boot_info->kernel_phys_base == 0x20000) {
		debugcon_putc('B');  // 'B' = BootInfo valid
	}

	// ============================================================
	// Test destructors (will run when objects go out of scope)
	// ============================================================
	// Expected: D1 (obj1 destructor), d (derived destructor)
	// But we halt before main returns, so destructors won't run

	// End marker: "===END"
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('E');
	debugcon_putc('N');
	debugcon_putc('D');

	// Halt
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
