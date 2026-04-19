/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 009 goal:
 *   Serial output: [BIG] Big kernel running @ 0x1000000
 *
 * The function:
 *   1. Initialises the serial port (COM1, 115200 8N1)
 *   2. Prints the milestone message to confirm we reached here
 *   3. Enters an idle halt loop
 */

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"

// ============================================================
// Forward declaration (filled by linker / boot.S)
// ============================================================
// BootInfo is defined in boot/boot_info.h but for the initial
// milestone we do not need it -- we just confirm the kernel runs.
// The full prototype will be: kernel_main(BootInfo* info)

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

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
