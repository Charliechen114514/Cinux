/**
 * @file kernel/arch/x86_64/usermode.cpp
 * @brief User-mode (Ring 3) transition support implementation
 *
 * Provides usermode_init() which configures the STAR MSR for SYSRET,
 * and launch_first_user() which sets up a minimal user address space
 * and transitions to Ring 3.
 */

#include "kernel/arch/x86_64/usermode.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
namespace cinux::arch {

// Assembly routine that configures STAR and EFER MSRs
extern "C" void usermode_init_asm();

// ============================================================
// Internal helpers
// ============================================================

namespace {

using cinux::mm::AddressSpace;
using cinux::mm::g_pmm;
using cinux::lib::kprintf;

/** User program machine code: cli; hlt; jmp .-2
 *
 * 0xFA        = cli          (clear interrupt flag -- Ring 0 only!)
 * 0xF4        = hlt          (halt -- also Ring 0 only)
 * 0xEB 0xFC   = jmp rel8 -4 (infinite loop back to cli)
 */
constexpr uint8_t kUserCode[] = {
    0xFA,                   // cli
    0xF4,                   // hlt
    0xEB, 0xFC,             // jmp -4 (back to cli)
};

/** Page flags for user-accessible, present, writable pages. */
constexpr uint64_t kUserPageFlags =
    FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

void usermode_init() {
    usermode_init_asm();
    kprintf("[USER] STAR/EFER MSRs configured for SYSRET.\n");
}

void launch_first_user() {
    kprintf("[USER] Setting up first user-mode program...\n");

    // Step 1: Create an isolated address space for the user process
    AddressSpace user_space;

    // Step 2: Allocate a physical page for user code
    uint64_t code_phys = g_pmm.alloc_page();
    if (code_phys == 0) {
        kprintf("[USER] FATAL: failed to allocate user code page\n");
        return;
    }

    // Step 3: Map the code page at USER_ENTRY_BASE with user-accessible flags
    if (!user_space.map(USER_ENTRY_BASE, code_phys, kUserPageFlags)) {
        kprintf("[USER] FATAL: failed to map user code page\n");
        return;
    }

    // Step 4: Copy the user program machine code into the code page
    // We need to write through the kernel's higher-half mapping.
    // Map the physical page temporarily in the kernel address space.
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    auto* code_virt = reinterpret_cast<uint8_t*>(code_phys + KERNEL_VMA);
    for (size_t i = 0; i < sizeof(kUserCode); i++) {
        code_virt[i] = kUserCode[i];
    }

    // Step 5: Allocate and map user stack pages
    // Stack grows downward, so we map pages below USER_STACK_TOP
    uint64_t stack_size = USER_STACK_PAGES * PAGE_SIZE;
    uint64_t stack_base = USER_STACK_TOP - stack_size;

    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            kprintf("[USER] FATAL: failed to allocate user stack page %u\n", i);
            return;
        }

        uint64_t virt = stack_base + i * PAGE_SIZE;
        if (!user_space.map(virt, phys, kUserPageFlags)) {
            kprintf("[USER] FATAL: failed to map user stack page at %p\n",
                    reinterpret_cast<void*>(virt));
            return;
        }
    }

    // Step 6: Copy kernel identity-mapped PDPT entries (e.g., 1 GB pages
    // for MMIO like the framebuffer) into the user PDPT so that the console
    // remains accessible after switching CR3.
    {
        auto* kern_pml4 = reinterpret_cast<const uint64_t*>(
            AddressSpace::kernel_pml4() + KERNEL_VMA);
        auto* user_pml4 = reinterpret_cast<uint64_t*>(
            user_space.pml4_phys() + KERNEL_VMA);

        if ((kern_pml4[0] & FLAG_PRESENT) && (user_pml4[0] & FLAG_PRESENT)) {
            auto* kern_pdpt = reinterpret_cast<const uint64_t*>(
                (kern_pml4[0] & ADDR_MASK) + KERNEL_VMA);
            auto* user_pdpt = reinterpret_cast<uint64_t*>(
                (user_pml4[0] & ADDR_MASK) + KERNEL_VMA);

            for (uint32_t i = 0; i < PT_ENTRIES; i++) {
                if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT)) {
                    user_pdpt[i] = kern_pdpt[i];
                }
            }
        }
    }

    // Step 7: Activate the user address space
    user_space.activate();
    kprintf("[USER] User address space activated (PML4 at phys %p).\n",
            reinterpret_cast<void*>(user_space.pml4_phys()));

    // Step 8: Set TSS.RSP0 so the CPU knows where to find the kernel stack
    // on privilege level changes (e.g., when an exception fires in Ring 3).
    // We use a simple approach: the current RSP is fine for this demo.
    uint64_t kernel_rsp0;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp0));
    GDT::tss_set_rsp0(kernel_rsp0);

    kprintf("[USER] Jumping to Ring 3: entry=%p stack=%p\n",
            reinterpret_cast<void*>(USER_ENTRY_BASE),
            reinterpret_cast<void*>(USER_STACK_TOP));

    // Step 9: Perform the SYSRET transition to Ring 3
    jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP, 0);

    // Should never reach here
    kprintf("[USER] ERROR: jump_to_usermode returned!\n");
}

}  // namespace cinux::arch
