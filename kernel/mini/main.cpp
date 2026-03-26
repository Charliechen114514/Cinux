/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include <boot_info.h>
#include "lib/kprintf.h"
#include "mm/pmm.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
	BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
	(void)boot_info_addr;

	// ============================================================
	// Kernel Entry Point
	// ============================================================
	kprintf("Cinux Mini Kernel v0.1.0\n");
	kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n", boot_info->entry_point,
			boot_info->kernel_phys_base);
	kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);
	for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
		const MemoryMapEntry* entry = &boot_info->mmap[i];
		kprintf("  [%u] base=0x%016x, length=0x%016x, type=%u, acpi=%u\n", i, entry->base,
				entry->length, entry->type, entry->acpi);
	}

	// TODO: Initialize kernel subsystems
	// TODO: Start scheduler

	// ============================================================
	// Initialize Physical Memory Manager
	// ============================================================
	using cinux::mini::mm::pmm::init;
	init(boot_info);

	// Halt
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
