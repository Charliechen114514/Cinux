/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 *
 * Milestone 009: Load the big kernel from disk and jump to it.
 * The mini kernel initialises hardware (GDT/IDT/PMM/ATA), reads
 * the big kernel ELF from disk into a staging buffer, loads its
 * PT_LOAD segments, and jumps to the entry point.
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include <boot_info.h>

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "big_kernel_loader.hpp"
#include "driver/ata.hpp"
#include "elf_loader.hpp"
#include "lib/kprintf.h"
#include "mm/pmm.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

/// Sector buffer aligned for DMA-safe access
static uint8_t g_sector_buf[512] __attribute__((aligned(16)));

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // ============================================================
    // Boot Info
    // ============================================================
    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n", boot_info->entry_point,
            boot_info->kernel_phys_base);
    kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);
    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        const MemoryMapEntry* entry = &boot_info->mmap[i];
        kprintf("  [%u] base=0x%016x, length=0x%016x, type=%u, acpi=%u\n", i, entry->base,
                entry->length, entry->type, entry->acpi);
    }

    // ============================================================
    // Initialize GDT + IDT
    // ============================================================
    kprintf("[INIT] Setting up GDT...\n");
    cinux::mini::arch::gdt_init();
    kprintf("[INIT] GDT loaded successfully.\n");

    kprintf("[INIT] Setting up IDT...\n");
    cinux::mini::arch::idt_init();
    kprintf("[INIT] IDT loaded successfully.\n");

    // ============================================================
    // Initialize PMM
    // ============================================================
    using cinux::mini::mm::pmm::init;
    init(boot_info);

    // ============================================================
    // Test: #BP breakpoint exception
    // ============================================================
    kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
    __asm__ volatile("int $3");
    kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");

    // ============================================================
    // Initialize ATA Disk Driver
    // ============================================================
    if (!cinux::mini::driver::ata::init()) {
        kprintf("[INIT] ERROR: ATA initialization failed!\n");
        while (1)
            __asm__ volatile("cli; hlt");
    }

    // ============================================================
    // Demo: Read MBR and verify boot signature
    // ============================================================
    kprintf("[DEMO] Reading MBR (LBA 0)...\n");
    if (cinux::mini::driver::ata::read(0, 1, g_sector_buf)) {
        uint16_t sig = static_cast<uint16_t>(g_sector_buf[510]) |
                       (static_cast<uint16_t>(g_sector_buf[511]) << 8);
        kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig,
                sig == 0xAA55 ? "(VALID)" : "(INVALID)");
    } else {
        kprintf("[DEMO] ERROR: Failed to read MBR\n");
    }

    // ============================================================
    // Demo: Read mini kernel from disk and check for ELF header
    // ============================================================
    kprintf("[DEMO] Reading mini kernel header (LBA 16)...\n");
    if (cinux::mini::driver::ata::read(16, 1, g_sector_buf)) {
        if (cinux::mini::elf_loader::parse_elf_header(g_sector_buf)) {
            kprintf("[DEMO] ELF header detected at disk LBA 16 (mini kernel)\n");
        } else {
            kprintf("[DEMO] No valid ELF header at LBA 16 (expected for flat binary)\n");
        }
    }

    // ============================================================
    // Load Big Kernel (009)
    // ============================================================
    // load_big_kernel() handles paging, overlap checks, and
    // the full two-phase ELF loading internally.
    uint64_t entry = cinux::mini::loader::load_big_kernel(cinux::mini::loader::BIG_KERNEL_LBA);
    if (entry == 0) {
        kprintf("[MINI] ERROR: Failed to load big kernel!\n");
        while (1)
            __asm__ volatile("cli; hlt");
    }

    kprintf("[MINI] Jumping to big kernel at 0x%p...\n", entry);

    // Indirect jump to the big kernel entry point (_start in boot.S).
    // _start sets up its own stack, clears BSS, runs global ctors,
    // and calls kernel_main().  It never returns.
    __asm__ volatile(
        "cli            \n\t"  // disable interrupts before handoff
        "jmp *%0        \n\t"
        :
        : "r"(entry)
        : "memory");

    // Should never reach here
    while (1)
        __asm__ volatile("cli; hlt");
}
