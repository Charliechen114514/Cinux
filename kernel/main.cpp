/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 025 goal:
 *   [AHCI] Read sector 0: 55 AA
 *
 * Milestone 026 goal:
 *   [RAMDISK] List files in embedded initrd (ustar)
 *
 * Initialisation order:
 *   1. Serial port (kprintf serial sink)
 *   2. GDT (segment descriptors + TSS with IST1 Double Fault stack)
 *   3. IDT (CPU exception vectors 0-14, #DF uses IST1)
 *   4. PIC (remap IRQ0-15 to vectors 0x20-0x2F)
 *   5. IRQ handlers (register IRQ stubs in IDT)
 *   6. PIT (configure channel 0 at 100 Hz)
 *   7. PMM (physical memory manager, bitmap allocator)
 *   8. VMM (virtual memory manager, 4-level paging)
 *   9. AddressSpace (save kernel PML4 for per-process spaces)
 *  10. Heap (kernel heap allocator, first-fit with coalescing)
 *  11. Framebuffer (map MMIO, init from BootInfo)
 *  12. Font (parse embedded PSF2)
 *  13. Console (init + register as kprintf sink)
 *  14. Keyboard (PS/2 controller init)
 *  15. Unmask IRQ0 + IRQ1, enable interrupts (sti)
 *  16. Usermode init (STAR/EFER MSRs for SYSRET)
 *  17. Syscall init (LSTAR, SFMASK, GS base, handler registration)
 *  18. Launch first user-mode program (Ring 3)
 *  19. PCI enumeration
 *  20. AHCI init + read sector 0 (MBR signature test)
 *  21. Ramdisk mount (parse embedded ustar initrd, list files)
 */

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/fs/ramdisk.hpp"
#include "kernel/drivers/video/console.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/address_space.hpp"

using cinux::arch::PIC;
using cinux::drivers::Console;
using cinux::drivers::Framebuffer;
using cinux::drivers::Keyboard;
using cinux::drivers::KeyEvent;
using cinux::drivers::PIT;
using cinux::drivers::PSFFont;

// BootInfo is placed at physical 0x7000 by the bootloader
static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

// Forward declarations for IRQ init (defined in irq_handlers.cpp)
extern "C" void irq_init();


/**
 * @brief Big kernel main entry point
 *
 * Called from boot.S after BSS clear and global ctors.
 *
 * @return This function should never return; the halt loop in
 *         boot.S catches it if it does.
 */
extern "C" void kernel_main() {
    // Step 1: Initialise the serial port used by kprintf
    cinux::lib::kprintf_init();

    // Step 2: Print the milestone message
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    // Step 3: Initialise the GDT (must come before IDT)
    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded (TSS with IST1 Double Fault stack).\n");

    // Step 4: Initialise the IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded (#DF uses IST1).\n");

    // Step 5: Initialise the PIC (remap IRQ0-7 -> 0x20-0x27,
    //         IRQ8-15 -> 0x28-0x2F, all masked)
    PIC::init();
    cinux::lib::kprintf("[BIG] PIC initialised.\n");

    // Step 6: Register IRQ handlers in the IDT (vectors 0x20-0x2F)
    irq_init();

    // Step 7: Initialise PIT channel 0 at 100 Hz (10 ms per tick)
    PIT::init(100);

    // Step 8: Trigger a software breakpoint to verify exception
    // handling still works after PIC/IRQ setup
    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    // Step 9: Initialise Physical Memory Manager
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    cinux::mm::g_pmm.init(*boot_info);

    // Step 10: Initialise Virtual Memory Manager
    cinux::mm::g_vmm.init();

    // Step 11: Save kernel PML4 for per-process address spaces
    cinux::mm::AddressSpace::init_kernel();

    // Step 12: Initialise kernel heap (64 KB initial region after kernel image)
    constexpr uint64_t HEAP_VIRT_BASE = 0xFFFF800000000000ULL;
    constexpr uint64_t HEAP_INITIAL_SIZE = 64 * 1024;
    cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);

    // Step 13: Initialise framebuffer from BootInfo
    Framebuffer fb;
    fb.init(*boot_info);
    cinux::lib::kprintf("[BIG] Framebuffer initialised: %ux%u %ubpp\n",
                        fb.width(), fb.height(), boot_info->fb_bpp);

    // Step 14: Parse embedded PSF2 font
    PSFFont font;
    font.init();
    cinux::lib::kprintf("[BIG] PSF2 font loaded: %ux%u\n",
                        font.width(), font.height());

    // Step 15: Initialise text console and register as kprintf sink
    Console console;
    console.init(fb, font, 0x00FFFFFF, 0x00000000);
    cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
    cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");

    // Step 16: Initialise the PS/2 keyboard controller
    Keyboard::init();

    // Step 17: Unmask IRQ0 (PIT timer) and IRQ1 (Keyboard), enable interrupts
    PIC::unmask(0);
    PIC::unmask(1);
    cinux::lib::kprintf("[BIG] IRQ0+IRQ1 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled.\n");

    // Step 18: Initialise user-mode support (STAR/EFER MSRs)
    cinux::arch::usermode_init();

    // Step 19: Initialise syscall infrastructure (LSTAR, SFMASK, dispatch table)
    cinux::arch::syscall_init();

    // Step 20: PCI enumeration
    cinux::lib::kprintf("[BIG] ===== Milestone 025: AHCI Driver =====\n");
    cinux::drivers::pci::PCI pci;
    pci.init();

    // Step 21: Find AHCI controller and initialise
    cinux::drivers::pci::PCIDevice ahci_dev;
    if (pci.find_ahci(ahci_dev)) {
        cinux::drivers::ahci::AHCI ahci;
        ahci.init(ahci_dev);

        // Step 22: Read sector 0 (MBR) and check boot signature
        uint64_t buf_phys = cinux::mm::g_pmm.alloc_page();
        if (buf_phys != 0) {
            constexpr uint64_t buf_virt = 0xFFFF800000300000ULL;
            constexpr uint64_t buf_flags = cinux::arch::FLAG_PRESENT
                                         | cinux::arch::FLAG_WRITABLE;
            cinux::mm::g_vmm.map(buf_virt, buf_phys, buf_flags);

            auto* buf = reinterpret_cast<uint8_t*>(buf_virt);
            for (uint32_t i = 0; i < 512; ++i) {
                buf[i] = 0;
            }

            if (ahci.read(0, 0, 1, buf_phys)) {
                cinux::lib::kprintf("[AHCI] Read sector 0: %02x %02x\n",
                                    buf[510], buf[511]);
            } else {
                cinux::lib::kprintf("[AHCI] Failed to read sector 0.\n");
            }
        }
    } else {
        cinux::lib::kprintf("[AHCI] No AHCI controller found.\n");
    }

    // Step 22: Mount the embedded initrd ramdisk (ustar archive)
    cinux::lib::kprintf("[BIG] ===== Milestone 026: Ramdisk (initrd) =====\n");
    cinux::fs::Ramdisk ramdisk;
    ramdisk.mount();

    // Step 23: Launch the first user-mode program (Ring 3)
    cinux::lib::kprintf("[BIG] ===== Milestone 023: Syscall from Ring 3 =====\n");
    cinux::arch::launch_first_user();

    cinux::lib::kprintf("[BIG] Returned from user mode launch (unexpected).\n");

    // Step 24: Keyboard poll loop -- echo keypresses to console + serial
    KeyEvent ev;
    while (1) {
        __asm__ volatile("hlt");

        // Drain all pending keyboard events
        while (Keyboard::poll(ev)) {
            if (ev.pressed && ev.ascii != 0) {
                console.putc(ev.ascii);
            }
        }
    }
}
