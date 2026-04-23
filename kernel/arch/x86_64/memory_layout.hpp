#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// Kernel virtual memory layout (0xFFFF8000_00000000+)
// ============================================================
// Regions are defined as (base, size) pairs.  Each subsequent
// region starts at the previous region's base + size.
// To add a new region, insert it here and bump the ones below.

constexpr uint64_t KMEM_BASE = 0xFFFF800000000000ULL;

// Heap: kernel heap allocator
constexpr uint64_t KMEM_HEAP_SIZE  = 0x8000000ULL;      // 128 MB
constexpr uint64_t KMEM_HEAP_BASE  = KMEM_BASE;

// MMIO: memory-mapped I/O (AHCI BAR5, etc.)
constexpr uint64_t KMEM_MMIO_SIZE  = 0x40000ULL;       // 256 KB
constexpr uint64_t KMEM_MMIO_BASE  = KMEM_HEAP_BASE + KMEM_HEAP_SIZE;

// Stacks: per-task kernel stacks (allocated upward)
constexpr uint64_t KMEM_STACK_BASE = KMEM_MMIO_BASE + KMEM_MMIO_SIZE;

// DMA: ad-hoc DMA buffers (sector reads, etc.)
constexpr uint64_t KMEM_DMA_SIZE   = 0x100000ULL;       // 1 MB
constexpr uint64_t KMEM_DMA_BASE   = KMEM_STACK_BASE + 0x100000ULL;

// ext2 DMA: ext2 filesystem block cache / DMA buffers
constexpr uint64_t KMEM_EXT2_DMA_SIZE = 0x100000ULL;    // 1 MB
constexpr uint64_t KMEM_EXT2_DMA_BASE = KMEM_DMA_BASE + KMEM_DMA_SIZE;

}  // namespace cinux::arch
