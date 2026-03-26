/**
 * @file kernel/mini/mm/pmm.h
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator
 *
 * Simple bitmap-based physical page allocator for the mini kernel.
 * Uses one bit per 4KB page to track available/used memory.
 */

#pragma once

#include <stdint.h>
#include "mm_defines.h"

namespace cinux::mini::mm::pmm {

// ============================================================
// Constants
// ============================================================
constexpr uint64_t PAGE_SIZE			  = 4_KB;			// 4KB pages
constexpr uint64_t MAX_MEMORY			= 4_GB;			// 4GB max supported
constexpr uint64_t MAX_PAGES			= MAX_MEMORY / PAGE_SIZE;		// 1M pages max
constexpr uint64_t BITMAP_SIZE		  = MAX_PAGES / 8;				// 128KB bitmap
constexpr uint64_t LOW_MEMORY_BOUNDARY  = 1_MB; // 1MB

// ============================================================
// Initialization
// ============================================================
/**
 * @brief Initialize the PMM from BootInfo
 * @param boot_info Pointer to BootInfo structure from bootloader
 *
 * Parses the E820 memory map, initializes the bitmap, and marks
 * reserved regions (kernel, bitmap itself, low 1MB) as used.
 */
void init(const void* boot_info);

// ============================================================
// Page Allocation
// ============================================================
/**
 * @brief Allocate a single physical page
 * @return Physical address of allocated page, 0 if OOM
 */
uint64_t alloc_page();

/**
 * @brief Free a single physical page
 * @param phys Physical address of page to free
 */
void free_page(uint64_t phys);

// ============================================================
// Statistics
// ============================================================
/**
 * @brief Get total number of free pages
 */
uint64_t free_page_count();

/**
 * @brief Get total number of pages in system
 */
uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
