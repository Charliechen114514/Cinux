/**
 * @file kernel/arch/x86_64/usermode.cpp
 * @brief User-mode (Ring 3) transition support implementation
 *
 * Provides usermode_init() which configures the STAR/EFER MSRs for SYSRET
 * and sets up the per-CPU GS data page used by syscall_entry for kernel
 * stack access.  Also provides jump_to_usermode() for entering Ring 3.
 */

#include "kernel/arch/x86_64/usermode.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/per_cpu.hpp"

namespace cinux::arch {

extern "C" void usermode_init_asm();

// ============================================================
// Internal helpers
// ============================================================

namespace {

using cinux::mm::g_pmm;
using cinux::lib::kprintf;

constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(static_cast<uint32_t>(value & 0xFFFFFFFF)),
                       "d"(static_cast<uint32_t>(value >> 32)));
}

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

void usermode_init() {
    usermode_init_asm();
    kprintf("[USER] STAR/EFER MSRs configured for SYSRET.\n");

    // Allocate the per-CPU data page used by syscall_entry.
    // gs:0 = kernel stack pointer (updated by scheduler on context switch)
    // gs:8 = user RSP scratch (saved/restored by syscall handler)
    // gs:16 = return value scratch
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

    uint64_t gs_phys = g_pmm.alloc_page();
    if (gs_phys == 0) {
        kprintf("[USER] FATAL: failed to allocate per-CPU GS data page\n");
        return;
    }

    auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
    gs_virt[0]    = 0;  // kernel stack — filled by scheduler on first context switch
    gs_virt[1]    = 0;

    write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);

    cinux::proc::g_per_cpu.gs_page_vaddr = gs_phys + KERNEL_VMA;

    kprintf("[USER] Per-CPU GS data page at %p (phys %p)\n",
            reinterpret_cast<void*>(gs_phys + KERNEL_VMA), reinterpret_cast<void*>(gs_phys));
}

}  // namespace cinux::arch
