/**
 * @file kernel/arch/x86_64/usermode.cpp
 * @brief User-mode (Ring 3) transition support implementation
 *
 * Provides usermode_init() which configures the STAR MSR for SYSRET,
 * and launch_first_user() which sets up a minimal user address space
 * and transitions to Ring 3.
 */

#include "kernel/arch/x86_64/usermode.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"


namespace cinux::arch {

extern "C" void usermode_init_asm();

extern "C" {
extern const uint8_t _binary_shell_bin_start[];
extern const uint8_t _binary_shell_bin_end[];
}  // extern "C"

// ============================================================
// Internal helpers
// ============================================================

namespace {

using cinux::mm::AddressSpace;
using cinux::mm::g_pmm;
using cinux::lib::kprintf;

constexpr uint64_t kUserPageFlags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

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
}

void launch_first_user() {
	kprintf("[USER] Setting up first user-mode program...\n");

	AddressSpace user_space;

	constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
	size_t			   user_size  = _binary_shell_bin_end - _binary_shell_bin_start;

	// Calculate how many code pages are needed
	size_t code_pages = (user_size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (code_pages == 0) {
		code_pages = 1;
	}

	for (size_t page = 0; page < code_pages; page++) {
		uint64_t code_phys = g_pmm.alloc_page();
		if (code_phys == 0) {
			kprintf("[USER] FATAL: failed to allocate user code page %u\n", page);
			return;
		}

		uint64_t virt = USER_ENTRY_BASE + page * PAGE_SIZE;
		if (!user_space.map(virt, code_phys, kUserPageFlags)) {
			kprintf("[USER] FATAL: failed to map user code page %u at %p\n",
					page, reinterpret_cast<void*>(virt));
			return;
		}

		// Copy the portion of shell binary that belongs to this page
		auto*  code_virt = reinterpret_cast<uint8_t*>(code_phys + KERNEL_VMA);
		size_t offset    = page * PAGE_SIZE;
		size_t chunk     = (offset + PAGE_SIZE <= user_size) ? PAGE_SIZE : (user_size - offset);
		for (size_t i = 0; i < chunk; i++) {
			code_virt[i] = _binary_shell_bin_start[offset + i];
		}
	}

	kprintf("[USER] Mapped %u code pages (%u bytes) for shell\n",
			static_cast<unsigned>(code_pages), static_cast<unsigned>(user_size));

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

	{
		auto* kern_pml4 =
			reinterpret_cast<const uint64_t*>(AddressSpace::kernel_pml4() + KERNEL_VMA);
		auto* user_pml4 = reinterpret_cast<uint64_t*>(user_space.pml4_phys() + KERNEL_VMA);

		if ((kern_pml4[0] & FLAG_PRESENT) && (user_pml4[0] & FLAG_PRESENT)) {
			auto* kern_pdpt =
				reinterpret_cast<const uint64_t*>((kern_pml4[0] & ADDR_MASK) + KERNEL_VMA);
			auto* user_pdpt = reinterpret_cast<uint64_t*>((user_pml4[0] & ADDR_MASK) + KERNEL_VMA);

			for (uint32_t i = 0; i < PT_ENTRIES; i++) {
				if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT)) {
					user_pdpt[i] = kern_pdpt[i];
				}
			}
		}
	}

	user_space.activate();
	kprintf("[USER] User address space activated (PML4 at phys %p).\n",
			reinterpret_cast<void*>(user_space.pml4_phys()));

	uint64_t kernel_rsp0;
	__asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp0));
	GDT::tss_set_rsp0(kernel_rsp0);

	uint64_t gs_phys = g_pmm.alloc_page();
	if (gs_phys == 0) {
		kprintf("[USER] FATAL: failed to allocate GS base page\n");
		return;
	}

	auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
	gs_virt[0]	  = kernel_rsp0;
	gs_virt[1]	  = 0;

	write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);

	kprintf("[USER] GS base setup: kernel_gsbase=%p (kernel_rsp=%p)\n",
			reinterpret_cast<void*>(gs_phys + KERNEL_VMA), reinterpret_cast<void*>(kernel_rsp0));

	kprintf("[USER] Jumping to Ring 3: entry=%p stack=%p\n",
			reinterpret_cast<void*>(USER_ENTRY_BASE), reinterpret_cast<void*>(USER_STACK_TOP));

	// Create a minimal Task so chdir/getcwd can read/write a per-process cwd
	static cinux::proc::Task shell_task{};
	shell_task.cwd[0] = '/';
	shell_task.cwd[1] = '\0';
	shell_task.state = cinux::proc::TaskState::Running;
	cinux::proc::Scheduler::set_current(&shell_task);

	jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);

	kprintf("[USER] ERROR: jump_to_usermode returned!\n");
}

}  // namespace cinux::arch
