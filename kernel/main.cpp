/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 011 goal:
 *   Serial prints "[TICK] uptime: Ns" once per second via PIT IRQ0.
 *
 * Initialisation order:
 *   1. Serial port (kprintf)
 *   2. GDT (segment descriptors + TSS)
 *   3. IDT (CPU exception vectors 0-14)
 *   4. PIC (remap IRQ0-15 to vectors 0x20-0x2F)
 *   5. IRQ handlers (register IRQ stubs in IDT)
 *   6. PIT (configure channel 0 at 100 Hz)
 *   7. Unmask IRQ0 + enable interrupts (sti)
 *   8. Idle halt loop
 */

#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/drivers/pit.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::drivers::PIT;

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

    // -- kprintf format regression test (after serial + GDT + IDT are up) --
    cinux::lib::kprintf("[KPRINTF] %%d: %d\n", 42);
    cinux::lib::kprintf("[KPRINTF] %%d negative: %d\n", -123);
    cinux::lib::kprintf("[KPRINTF] %%u: %u\n", 4294967295u);
    cinux::lib::kprintf("[KPRINTF] %%x: %x\n", 0xDEADBEEFu);
    cinux::lib::kprintf("[KPRINTF] %%X: %X\n", 0xDEADBEEFu);
    cinux::lib::kprintf("[KPRINTF] %%08x: %08x\n", 0xDEADu);
    cinux::lib::kprintf("[KPRINTF] %%10d: %10d\n", 42);
    cinux::lib::kprintf("[KPRINTF] %%-10d: %-10d|\n", 42);
    cinux::lib::kprintf("[KPRINTF] %%s: %s\n", "hello");
    cinux::lib::kprintf("[KPRINTF] %%-10s: %-10s|\n", "hi");
    cinux::lib::kprintf("[KPRINTF] %%p: %p\n", (void*)0x1234ABCD5678ull);
    cinux::lib::kprintf("[KPRINTF] %%c: %c\n", 'Z');
    cinux::lib::kprintf("[KPRINTF] %%%%: %%\n");
    cinux::lib::kprintf("[KPRINTF] %%010u: %010u\n", 42u);
    cinux::lib::kprintf("[KPRINTF] mix: %s n=%d hex=%08x ptr=%p\n",
                        "test", 99, 0xCAFEBABEu, (void*)0x1ull);
    cinux::lib::kprintf("[KPRINTF] all format tests done.\n");

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

    // Step 9: Unmask IRQ0 (PIT timer) and enable interrupts
    PIC::unmask(0);
    cinux::lib::kprintf("[BIG] IRQ0 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled. Entering idle loop.\n");

    // Idle loop: halt and wait for the next interrupt
    while (1) {
        __asm__ volatile("hlt");
    }
}
