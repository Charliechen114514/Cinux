/**
 * @file kernel/mm/vmm.cpp
 * @brief Virtual Memory Manager implementation
 */

#include "kernel/mm/vmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"

namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

// ============================================================
// Global instance
// ============================================================

VMM g_vmm;

// ============================================================
// Internal helpers
// ============================================================
using namespace cinux::arch;

namespace {

/** Convert a physical address to a virtual address via the higher-half mapping. */
PageEntry* phys_to_virt(uint64_t phys) {
	return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

/**
 * @brief Walk one level of the page table, allocating if needed
 *
 * @param table   Pointer to the current-level table (virtual address)
 * @param index   Index into this table
 * @param should_alloc  Whether to allocate a new table if the entry is absent
 * @return Pointer to the next-level table, or nullptr on allocation failure
 */
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc, uint64_t user_flag = 0) {
	PageEntry& entry = table[index];

	if (entry.is_present()) {
		if (entry.huge) {
			if (!should_alloc) {
				return nullptr;
			}

			uint64_t big_phys  = entry.phys_addr();
			uint64_t big_flags = entry.raw & ~ADDR_MASK;

			uint64_t new_page = cinux::mm::g_pmm.alloc_page();
			if (new_page == 0) {
				return nullptr;
			}

			auto* new_table = phys_to_virt(new_page);
			for (uint32_t i = 0; i < PT_ENTRIES; i++) {
				new_table[i].raw =
					(big_phys + static_cast<uint64_t>(i) * PAGE_SIZE) | (big_flags & ~FLAG_HUGE);
			}

			entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
		}
		return phys_to_virt(entry.phys_addr());
	}

	if (!should_alloc) {
		return nullptr;
	}

	uint64_t new_page = cinux::mm::g_pmm.alloc_page();
	if (new_page == 0) {
		return nullptr;
	}

	auto* new_table = phys_to_virt(new_page);
	for (uint32_t i = 0; i < PT_ENTRIES; i++) {
		new_table[i].raw = 0;
	}

	entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
	return new_table;
}

}  // anonymous namespace

// ============================================================
// VMM implementation
// ============================================================

void VMM::init() {
	kernel_pml4_ = cinux::arch::read_cr3();
	cinux::lib::kprintf("[VMM] Initialised, kernel PML4 at phys %p\n",
						reinterpret_cast<void*>(kernel_pml4_));
}

bool VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
	auto g = lock_.guard();
	(void)g;

	uint64_t pml4_phys	= pml4 ? *pml4 : kernel_pml4_;
	auto*	 pml4_table = phys_to_virt(pml4_phys);
	uint64_t user_flag	= flags & FLAG_USER;

	auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
	if (!pdpt)
		return false;

	auto* pd = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
	if (!pd)
		return false;

	auto* pt = walk_level(pd, PD_INDEX(virt), true, user_flag);
	if (!pt)
		return false;

	uint64_t pt_idx = PT_INDEX(virt);
	pt[pt_idx].raw	= (phys & ADDR_MASK) | (flags & ~ADDR_MASK);

	cinux::arch::flush_tlb(virt);
	return true;
}

void VMM::unmap(uint64_t virt, uint64_t* pml4) {
	auto g = lock_.guard();
	(void)g;

	uint64_t pml4_phys	= pml4 ? *pml4 : kernel_pml4_;
	auto*	 pml4_table = phys_to_virt(pml4_phys);
	auto*	 pdpt		= walk_level(pml4_table, PML4_INDEX(virt), false);
	if (!pdpt)
		return;

	auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
	if (!pd)
		return;

	auto* pt = walk_level(pd, PD_INDEX(virt), false);
	if (!pt)
		return;

	uint64_t pt_idx = PT_INDEX(virt);
	pt[pt_idx].raw	= 0;

	cinux::arch::flush_tlb(virt);
}

uint64_t VMM::translate(uint64_t virt, uint64_t* pml4) {
	uint64_t pml4_phys	= pml4 ? *pml4 : kernel_pml4_;
	auto*	 pml4_table = phys_to_virt(pml4_phys);
	auto*	 pdpt		= walk_level(pml4_table, PML4_INDEX(virt), false);
	if (!pdpt)
		return 0;

	auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
	if (!pd)
		return 0;

	auto* pt = walk_level(pd, PD_INDEX(virt), false);
	if (!pt)
		return 0;

	PageEntry& entry = pt[PT_INDEX(virt)];
	if (!entry.is_present())
		return 0;

	uint64_t offset = virt & (PAGE_SIZE - 1);
	return entry.phys_addr() | offset;
}

uint64_t VMM::kernel_pml4() const {
	return kernel_pml4_;
}

}  // namespace cinux::mm
