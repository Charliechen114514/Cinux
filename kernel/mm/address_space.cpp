/**
 * @file kernel/mm/address_space.cpp
 * @brief Per-process virtual address space management implementation
 */

#include "kernel/mm/address_space.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

// ============================================================
// Static member initialisation
// ============================================================

uint64_t AddressSpace::kernel_pml4_ = 0;

// ============================================================
// Constants
// ============================================================

using namespace cinux::arch;

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

// PML4 entry indices that belong to user space (lower half)
constexpr uint32_t USER_PML4_START = 0;
constexpr uint32_t USER_PML4_END   = 256;

// Recursion levels for subtree freeing:
//   PML4 -> PDPT (level 3) -> PD (level 2) -> PT (level 1)
constexpr int LEVEL_PDPT = 3;
constexpr int LEVEL_PD   = 2;
constexpr int LEVEL_PT   = 1;

// ============================================================
// Internal helpers
// ============================================================

namespace {

/** Convert a physical address to a virtual address via the higher-half mapping. */
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

}  // anonymous namespace

// ============================================================
// Static initialisation
// ============================================================

void AddressSpace::init_kernel() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[AS] Kernel PML4 saved at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}

// ============================================================
// Construction
// ============================================================

AddressSpace::AddressSpace() {
    // Step 1: Allocate a fresh PML4 page
    pml4_phys_ = g_pmm.alloc_page();
    if (pml4_phys_ == 0) {
        cinux::lib::kprintf("[AS] FATAL: failed to allocate PML4 page\n");
        return;
    }

    // Step 2: Zero the entire PML4
    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        pml4[i].raw = 0;
    }

    auto* kern_pml4 = phys_to_virt(kernel_pml4_);
    for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
        pml4[i].raw = kern_pml4[i].raw;
    }
}

// ============================================================
// Destruction
// ============================================================

AddressSpace::~AddressSpace() {
    // Nothing to free if the PML4 was never allocated (or was moved out)
    if (pml4_phys_ == 0) {
        return;
    }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
        if (pml4[i].is_present()) {
            free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
        }
    }

    // Step 2: Free the PML4 page itself
    g_pmm.free_page(pml4_phys_);
    pml4_phys_ = 0;
}

// ============================================================
// Move operations
// ============================================================

AddressSpace::AddressSpace(AddressSpace&& other) noexcept
    : pml4_phys_(other.pml4_phys_) {
    other.pml4_phys_ = 0;
}

AddressSpace& AddressSpace::operator=(AddressSpace&& other) noexcept {
    if (this != &other) {
        // Free our current resources
        if (pml4_phys_ != 0) {
            auto* pml4 = phys_to_virt(pml4_phys_);
            for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
                if (pml4[i].is_present()) {
                    free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
                }
            }
            g_pmm.free_page(pml4_phys_);
        }

        // Take ownership of the other's PML4
        pml4_phys_ = other.pml4_phys_;
        other.pml4_phys_ = 0;
    }
    return *this;
}

// ============================================================
// Page table operations
// ============================================================

bool AddressSpace::map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return g_vmm.map(virt, phys, flags, &pml4_phys_);
}

void AddressSpace::unmap(uint64_t virt) {
    g_vmm.unmap(virt, &pml4_phys_);
}

uint64_t AddressSpace::translate(uint64_t virt) {
    return g_vmm.translate(virt, &pml4_phys_);
}

void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);
}

// ============================================================
// Accessors
// ============================================================

uint64_t AddressSpace::pml4_phys() const {
    return pml4_phys_;
}

uint64_t AddressSpace::kernel_pml4() {
    return kernel_pml4_;
}

// ============================================================
// Internal helpers
// ============================================================

void AddressSpace::free_subtree(uint64_t table_phys, int level) {
    auto* table = phys_to_virt(table_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!table[i].is_present()) {
            continue;
        }

        // Stop recursion at PT level -- PT entries point to data pages
        // which are NOT owned by the address space infrastructure
        if (level > LEVEL_PT) {
            free_subtree(table[i].phys_addr(), level - 1);
        }

        // Free the page table page at this level
        g_pmm.free_page(table[i].phys_addr());
    }
}

}  // namespace cinux::mm
