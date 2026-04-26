/**
 * @file kernel/arch/x86_64/paging.hpp
 * @brief x86_64 4-level paging structures and helpers
 *
 * Defines the PageEntry union for manipulating PML4/PDPT/PD/PT entries
 * and declares TLB flush / CR3 access helpers plus the legacy MMIO
 * mapping function.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"

namespace cinux::arch {

// ============================================================
// PageEntry union
// ============================================================

union PageEntry {
    uint64_t raw;

    struct {
        uint64_t present : 1;
        uint64_t writable : 1;
        uint64_t user : 1;
        uint64_t pwt : 1;
        uint64_t pcd : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t huge : 1;
        uint64_t global : 1;
        uint64_t _avail : 3;
        uint64_t addr : 40;
        uint64_t _avail2 : 11;
        uint64_t nx : 1;
    };

    uint64_t phys_addr() const { return raw & ADDR_MASK; }

    void set_phys_addr(uint64_t phys) { raw = (raw & ~ADDR_MASK) | (phys & ADDR_MASK); }

    bool is_present() const { return (raw & FLAG_PRESENT) != 0; }
};

static_assert(sizeof(PageEntry) == 8, "PageEntry must be 8 bytes");

// ============================================================
// TLB flush helpers
// ============================================================

inline void flush_tlb(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

inline void flush_tlb_all() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

inline uint64_t read_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

inline void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// ============================================================
// MMIO mapping (legacy, used by framebuffer driver)
// ============================================================

void map_mmio(uint64_t phys, uint64_t size);

}  // namespace cinux::arch
