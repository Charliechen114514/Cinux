/**
 * @file kernel/mm/vmm.hpp
 * @brief Virtual Memory Manager
 *
 * Provides map / unmap / translate operations over the 4-level page table
 * hierarchy (PML4 -> PDPT -> PD -> PT -> Page).  When intermediate tables
 * are missing during a map, new pages are allocated from the PMM and
 * zeroed automatically.
 *
 * The VMM class is an instance object (not a static singleton) so that
 * future milestones can manage multiple address spaces.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stdint.h>

namespace cinux::mm {

class VMM {
public:
    /**
     * @brief Initialise VMM with the current CR3 as the kernel PML4
     *
     * Reads CR3 and stores the kernel PML4 physical address for use as
     * the default page table root in subsequent map/unmap/translate calls.
     */
    void init();

    /**
     * @brief Map a single 4 KB virtual page to a physical page
     *
     * Walks the 4-level table from PML4 to PT, allocating and zeroing
     * intermediate tables from the PMM as needed.  Sets the final PT
     * entry to (phys | flags).
     *
     * @param virt   Virtual address to map (page-aligned recommended)
     * @param phys   Physical address to map to (must be page-aligned)
     * @param flags  Combination of FLAG_PRESENT, FLAG_WRITABLE, FLAG_USER, etc.
     * @param pml4   Optional PML4 physical address; nullptr = use kernel PML4
     * @return true on success, false if PMM allocation failed
     */
    bool map(uint64_t virt, uint64_t phys, uint64_t flags,
             uint64_t* pml4 = nullptr);

    /**
     * @brief Unmap a single 4 KB virtual page
     *
     * Clears the PT entry for @p virt and flushes the TLB for that page.
     * Does NOT free the physical page that was mapped -- the caller is
     * responsible for returning it to the PMM.
     *
     * @param virt  Virtual address to unmap
     * @param pml4  Optional PML4 physical address; nullptr = use kernel PML4
     */
    void unmap(uint64_t virt, uint64_t* pml4 = nullptr);

    /**
     * @brief Translate a virtual address to its physical counterpart
     *
     * Walks the 4-level page table and returns the physical page base
     * plus the in-page offset of @p virt.  Returns 0 if the address is
     * not mapped.
     *
     * @param virt  Virtual address to translate
     * @param pml4  Optional PML4 physical address; nullptr = use kernel PML4
     * @return Physical address, or 0 if not present
     */
    uint64_t translate(uint64_t virt, uint64_t* pml4 = nullptr);

    /** Get the saved kernel PML4 physical address. */
    uint64_t kernel_pml4() const;

private:
    uint64_t kernel_pml4_{};
};

/// Global VMM instance.
extern VMM g_vmm;

}  // namespace cinux::mm
