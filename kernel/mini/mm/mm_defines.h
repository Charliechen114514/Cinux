/**
 * @file kernel/mini/mm/mm_defines.h
 * @brief Memory Management Common Definitions
 *
 * Central header for memory-related utilities and literals.
 * Include this file to access memory size literals and common definitions.
 */

#pragma once

#include <stdint.h>

// Import memory literals (_KB, _MB, _GB, _TB)
#include "memory_literals.h"

namespace cinux::mini::mm {

// ============================================================
// Common Page Size Definitions
// ============================================================
constexpr uint64_t PAGE_SIZE_4K = 4_KB;   // 4096 bytes
constexpr uint64_t PAGE_SIZE_2M = 2_MB;   // 2097152 bytes
constexpr uint64_t PAGE_SIZE_1G = 1_GB;   // 1073741824 bytes

// ============================================================
// Memory Alignment Helpers
// ============================================================
/**
 * @brief Align address up to specified alignment
 * @param addr Address to align
 * @param align Alignment boundary (must be power of 2)
 * @return Aligned address
 */
constexpr uint64_t align_up(uint64_t addr, uint64_t align) {
	return (addr + align - 1) & ~(align - 1);
}

/**
 * @brief Align address down to specified alignment
 * @param addr Address to align
 * @param align Alignment boundary (must be power of 2)
 * @return Aligned address
 */
constexpr uint64_t align_down(uint64_t addr, uint64_t align) {
	return addr & ~(align - 1);
}

/**
 * @brief Check if address is aligned to specified boundary
 * @param addr Address to check
 * @param align Alignment boundary
 * @return true if aligned, false otherwise
 */
constexpr bool is_aligned(uint64_t addr, uint64_t align) {
	return (addr & (align - 1)) == 0;
}

} // namespace cinux::mini::mm
