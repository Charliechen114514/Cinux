/**
 * @file kernel/mm/address_space.hpp
 * @brief Per-process virtual address space management
 *
 * Encapsulates an independent PML4 root and provides isolated user-space
 * page table management.  Each AddressSpace instance owns a freshly
 * allocated PML4 whose kernel half (entries 256-511) mirrors the global
 * kernel mapping, while the user half (entries 0-255) is private.
 *
 * Copy construction and copy assignment are deleted to prevent accidental
 * sharing of physical page table pages.
 *
 * Depends on: PMM (for page allocation), VMM (for map/unmap/translate
 * helpers), and x86_64 paging primitives.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stdint.h>

namespace cinux::mm {

class AddressSpace {
public:
    // ============================================================
    // Construction / Destruction
    // ============================================================

    /**
     * @brief Construct an isolated address space
     *
     * Allocates a new PML4 page from the PMM, zeroes it, then copies
     * kernel-space entries (PML4[256..511]) from the saved kernel PML4
     * so that the kernel mapping is visible in every address space.
     *
     * @note Requires that init_kernel() has been called beforehand.
     */
    AddressSpace();

    /**
     * @brief Destroy the address space and free all user-space page tables
     *
     * Walks PML4[0..255] and, for each present entry, recursively frees
     * the entire subtree (PDPT -> PD -> PT) back to the PMM.  Finally
     * frees the PML4 page itself.
     */
    ~AddressSpace();

    // Disable copy -- each AddressSpace owns exclusive physical pages
    AddressSpace(const AddressSpace&)            = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    // Allow move -- transfers ownership of pml4_phys_
    AddressSpace(AddressSpace&& other) noexcept;
    AddressSpace& operator=(AddressSpace&& other) noexcept;

    // ============================================================
    // Static initialisation
    // ============================================================

    /**
     * @brief Read the current CR3 and save it as the kernel PML4
     *
     * Must be called once during boot, after the initial page tables
     * are set up but before any AddressSpace instance is created.
     */
    static void init_kernel();

    // ============================================================
    // Page table operations
    // ============================================================

    /**
     * @brief Map a single 4 KB virtual page within this address space
     *
     * Delegates to VMM using this space's PML4 as the root.
     *
     * @param virt   Virtual address to map (page-aligned recommended)
     * @param phys   Physical address to map to (must be page-aligned)
     * @param flags  Combination of FLAG_PRESENT, FLAG_WRITABLE, etc.
     * @return true on success, false on allocation failure
     */
    bool map(uint64_t virt, uint64_t phys, uint64_t flags);

    /**
     * @brief Unmap a single 4 KB virtual page within this address space
     *
     * @param virt  Virtual address to unmap
     */
    void unmap(uint64_t virt);

    /**
     * @brief Translate a virtual address to physical within this space
     *
     * @param virt  Virtual address to look up
     * @return Physical address, or 0 if not mapped
     */
    uint64_t translate(uint64_t virt);

    /**
     * @brief Activate this address space (load CR3)
     *
     * Writes this space's PML4 physical address into CR3, making it
     * the active page table root.  Flushes the TLB implicitly.
     */
    void activate();

    // ============================================================
    // Accessors
    // ============================================================

    /** Get the physical address of this space's PML4 root. */
    uint64_t pml4_phys() const;

    /** Get the saved kernel PML4 physical address. */
    static uint64_t kernel_pml4();

private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Recursively free all page table pages under a given entry
     *
     * @param table_virt  Virtual address of the current-level table
     * @param level       Current level: 3 = PDPT, 2 = PD, 1 = PT
     */
    void free_subtree(uint64_t table_phys, int level);

    // ============================================================
    // Data members
    // ============================================================

    /** Physical address of this address space's PML4 root. */
    uint64_t pml4_phys_{};

    /** Saved kernel PML4 physical address (populated by init_kernel). */
    static uint64_t kernel_pml4_;
};

}  // namespace cinux::mm
