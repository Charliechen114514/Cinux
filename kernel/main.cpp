/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 014 goal:
 *   Keyboard input with screen + serial echo on keypress.
 *
 * Initialisation order:
 *   1. Serial port (kprintf serial sink)
 *   2. GDT (segment descriptors + TSS)
 *   3. IDT (CPU exception vectors 0-14)
 *   4. PIC (remap IRQ0-15 to vectors 0x20-0x2F)
 *   5. IRQ handlers (register IRQ stubs in IDT)
 *   6. PIT (configure channel 0 at 100 Hz)
 *   7. Framebuffer (map MMIO, init from BootInfo)
 *   8. Font (parse embedded PSF2)
 *   9. Console (init + register as kprintf sink)
 *  10. Keyboard (PS/2 controller init)
 *  11. Unmask IRQ0 + IRQ1, enable interrupts (sti)
 *  12. Keyboard poll loop: echo keypresses to console + serial
 */

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/drivers/video/console.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/lib/kprintf.hpp"

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
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    // Step 4: Initialise the IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

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

    // Step 9: Initialise framebuffer from BootInfo
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    Framebuffer fb;
    fb.init(*boot_info);
    cinux::lib::kprintf("[BIG] Framebuffer initialised: %ux%u %ubpp\n",
                        fb.width(), fb.height(), boot_info->fb_bpp);

    // Step 10: Parse embedded PSF2 font
    PSFFont font;
    font.init();
    cinux::lib::kprintf("[BIG] PSF2 font loaded: %ux%u\n",
                        font.width(), font.height());

    // Step 11: Initialise text console and register as kprintf sink
    Console console;
    console.init(fb, font, 0x00FFFFFF, 0x00000000);
    cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
    cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");

    // Step 10: Initialise the PS/2 keyboard controller
    Keyboard::init();

    // Step 11: Unmask IRQ0 (PIT timer) and IRQ1 (Keyboard), enable interrupts
    PIC::unmask(0);
    PIC::unmask(1);
    cinux::lib::kprintf("[BIG] IRQ0+IRQ1 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled. Keyboard echo active.\n");

    // Step 12: Keyboard poll loop -- echo keypresses to console + serial
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
