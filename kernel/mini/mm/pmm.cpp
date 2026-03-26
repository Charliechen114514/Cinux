/**
 * @file kernel/mini/mm/pmm.cpp
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator Implementation
 */

#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#include "../../../boot/boot_info.h"
#include "lib/kprintf.h"

namespace cinux::mini::mm::pmm {

// ============================================================
// Internal State
// ============================================================
static uint64_t s_total_pages		  = 0;	  // Total pages in system
static uint64_t s_free_pages		  = 0;	  // Free pages available
static uint64_t s_highest_page		  = 0;	  // Highest page index managed
static uint8_t	s_bitmap[BITMAP_SIZE] = {0};  // Bitmap storage

// External symbols from linker (use &symbol to get the value)
extern "C" {
	extern char __kernel_size;     // Kernel size in bytes (from linker.ld)
	extern char __mini_kernel_end; // End of kernel (from linker.ld)
}

namespace {

// ============================================================
// Bitmap Operations
// ============================================================
void set_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	s_bitmap[byte_idx] |= (1U << bit_idx);
}

void clear_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

bool test_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}

int64_t find_first_free() {
	// Scan bitmap for first zero bit
	for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
		if (s_bitmap[byte_idx] != 0xFF) {
			// Found a byte with at least one free bit
			uint8_t byte = s_bitmap[byte_idx];
			for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
				if ((byte & (1U << bit_idx)) == 0) {
					return static_cast<int64_t>(byte_idx * 8 + bit_idx);
				}
			}
		}
	}
	return -1;	// No free pages
}

// ============================================================
// Memory Region Management
// ============================================================
void mark_region_used(uint64_t phys, uint64_t length) {
	uint64_t start_page = phys / PAGE_SIZE;
	uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (page < MAX_PAGES && !test_bit(page)) {
			set_bit(page);
			s_free_pages--;
		}
	}
}

void mark_region_free(uint64_t phys, uint64_t length) {
	uint64_t start_page = phys / PAGE_SIZE;
	uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (page < MAX_PAGES) {
			clear_bit(page);
			s_free_pages++;
		}
	}
}

}  // anonymous namespace

// ============================================================
// Initialization
// ============================================================
void init(const void* boot_info) {
	const BootInfo* info = static_cast<const BootInfo*>(boot_info);

	// Step 1: Initialize bitmap - mark all pages as used
	for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
		s_bitmap[i] = 0xFF;
	}
	s_total_pages  = 0;
	s_free_pages   = 0;
	s_highest_page = 0;

	// Step 2: Parse E820 memory map and mark available regions as free
	for (uint32_t i = 0; i < info->mmap_count; i++) {
		const MemoryMapEntry* entry = &info->mmap[i];

		// Only process usable memory (type = 1)
		if (entry->type != 1) {
			continue;
		}

		uint64_t base	= entry->base;
		uint64_t length = entry->length;

		// Update highest page
		uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
		if (end_page > s_highest_page) {
			s_highest_page = end_page;
			if (s_highest_page > MAX_PAGES) {
				s_highest_page = MAX_PAGES;
			}
		}

		// Filter out low 1MB (reserved by bootloader)
		if (base < LOW_MEMORY_BOUNDARY) {
			if (length <= LOW_MEMORY_BOUNDARY - base) {
				// Entire region is in low 1MB, skip it
				continue;
			}
			// Partial overlap: adjust base and length
			length -= (LOW_MEMORY_BOUNDARY - base);
			base = LOW_MEMORY_BOUNDARY;
		}

		// Align to page boundaries
		uint64_t aligned_base	= (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		uint64_t aligned_length = length - (aligned_base - base);

		if (aligned_length < PAGE_SIZE) {
			continue;
		}

		// Mark pages as free
		mark_region_free(aligned_base, aligned_length);
	}

	s_total_pages = s_highest_page;

	// Step 3: Mark kernel region as used
	// Use linker-provided __kernel_size (note: &__kernel_size gives the value)
	uint64_t kernel_phys = info->kernel_phys_base;
	uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
	lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n", kernel_phys,
				 kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
	mark_region_used(kernel_phys, kernel_size);

	// Step 4: Mark bootloader regions as used (0x0 - 0x10000)
	lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
				 0x10000 / PAGE_SIZE);
	mark_region_used(0x0, 0x10000);

	// Debug output
	lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n", s_total_pages,
				 (s_total_pages * PAGE_SIZE) / 1_MB, s_free_pages,
				 (s_free_pages * PAGE_SIZE) / 1_MB);
}

// ============================================================
// Page Allocation
// ============================================================
uint64_t alloc_page() {
	int64_t page_idx = find_first_free();
	if (page_idx < 0) {
		return 0;  // OOM
	}

	set_bit(static_cast<uint64_t>(page_idx));
	s_free_pages--;

	return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}

void free_page(uint64_t phys) {
	if (phys == 0) {
		return;	 // Null address, ignore
	}

	uint64_t page_idx = phys / PAGE_SIZE;
	if (page_idx >= MAX_PAGES) {
		return;	 // Invalid address
	}

	if (test_bit(page_idx)) {
		clear_bit(page_idx);
		s_free_pages++;
	}
}

// ============================================================
// Statistics
// ============================================================
uint64_t free_page_count() {
	return s_free_pages;
}

uint64_t total_page_count() {
	return s_total_pages;
}

}  // namespace cinux::mini::mm::pmm
