/**
 * @file boot/stage2.S
 * @brief Cinux Stage2 Bootloader - VESA Initialization
 *
 * This file is loaded by MBR to 0x8000 and continues the boot process.
 * Responsibilities (for this milestone):
 *   - Print "Stage2 OK" message
 *   - Enable A20 line
 *   - Get VESA controller information
 *   - Get VESA mode information (0x118: 1024x768x32)
 *   - Set VESA video mode with linear framebuffer
 *   - Save framebuffer information to fixed location
 *
 * AT&T Syntax: source, destination order
 * Registers: % prefix, Immediates: $ prefix
 */

.section .text
.code16                      // 16-bit real mode code
.global _start                // Entry point

// ============================================================
// External functions from common/serial.S
// ============================================================
.extern print_string
.extern panic
.extern enable_a20
.extern vesa_get_controller_info
.extern vesa_get_mode_info
.extern vesa_set_mode
.extern vesa_save_framebuffer_info

// ============================================================
// Constants from common/serial.S
// ============================================================
.set VBE_FB_INFO_BLOCK,         0x6400
.set STAGE2_STACK_BASEADDR,     0xFFFE

// Framebuffer info layout offsets
.set FB_INFO_PHYS_ADDR,         0       // Physical base address (8 bytes)
.set FB_INFO_PITCH,             8       // Bytes per scan line (4 bytes)
.set FB_INFO_WIDTH,             12      // Width in pixels (2 bytes) 

// ============================================================
// Function: _start
// Responsibility: Stage2 entry point, called from MBR
// Input: None (execution starts here)
// Output: Never returns (continues to next stage)
// ============================================================
_start:
    cli                             // Disable interrupts during critical setup

    // Reset all registers again...
    movw %cs, %ax                 // Tricks: Xor one number with itself get 0
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    movw $0x900, %ax    // 0x900 << 4 = 0x9000
    movw %ax, %ss

    // [→sp] Stack at bottom of stage2
    movw $STAGE2_STACK_BASEADDR, %sp

    sti                             // Re-enable interrupts after stack is set

    // Mark Once, see the issue
    movw $(msg_stage2_ok), %si
    call print_string

    // Enable A20 line
    call enable_a20

    // Get VESA controller information
    call vesa_get_controller_info   // [→0x6000] Get VBE info block

    // Get VESA mode information for 0x118
    call vesa_get_mode_info         // [→0x6200] Get mode info block

    movw $(msg_mode_info_ok), %si
    call print_string               // "Mode info OK" (still in text mode!)

    // Set VESA video mode with linear framebuffer
    call vesa_set_mode              // [→video] Set 1024x768x32 mode (SCREEN CLEARS HERE)

    // Save framebuffer information to 0x6400
    // Layout: addr(8B) + pitch(4B) + w(2B) + h(2B) = 16B
    call vesa_save_framebuffer_info // [→0x6400] Save FB info

    cli                           // close the inter again
    // ============================================================
    // Switch to Protected Mode
    // ============================================================
    // IMPORTANT: lgdt in .code32 uses absolute address, but in real mode
    // CPU calculates physical address as DS*16 + offset. We must set DS=0
    // so that the address calculation is correct: 0*16 + 0x8160 = 0x8160
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)

    // 1. Load GDT pointer (with DS=0, physical address = 0x8160)
    lgdt gdt_ptr                  // Load GDTR from absolute address

    // 2. Set PE bit in CR0
    movl %cr0, %eax               // Read CR0
    orb $0x1, %al                 // Set PE bit (bit 0)
    movl %eax, %cr0               // Write CR0

    // 3. Far jump - manually construct for correct 16-bit encoding
    // ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>
    ljmp $0x08, $pm_entry

// ============================================================
// Protected Mode Entry Point
// ============================================================
.code32                          // Now in 32-bit protected mode
pm_entry:
    // 4. Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 5. Set up new stack
    movl $0x90000, %esp           // New stack in protected mode

    // 6. Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // 7. Infinite halt (we're in protected mode now)
    cli                             // Disable interrupts
.pm_halt:
    hlt
    jmp .pm_halt

// ============================================================
// Error Handlers
// ============================================================
a20_failed:
    jmp panic

// ============================================================
// GDT (Global Descriptor Table) - in .rodata section
// ============================================================
.section .gdt,"a"
.align 8                          // Align to 8 bytes (2^3)

gdt:
gdt_null:
    .quad 0                        // Null descriptor (required)

gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0 (= 0x8000)
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Present, DPL=0, Code, Executable, Readable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_data:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0 (= 0x8000)
    .byte 0x00                     // Base 23:16
    .byte 0x92                     // Present, DPL=0, Data, Writable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_end:

gdt_ptr:
    .word (gdt_end - gdt - 1) // Limit = sizeof(GDT) - 1 = 23 (3 descriptors * 8 - 1)
    .long gdt                 // Linear address of GDT base

// ============================================================
// Data Section
// ============================================================
.section .data

// Stage2 OK message
msg_stage2_ok:
    .asciz "Stage2 OK\r\n"

msg_mode_info_ok:
    .asciz "Mode info OK, switching...\r\n"

// VESA success message
msg_vesa_ok:
    .asciz "VESA: Mode 1024x768x32 set\r\n"

// ============================================================
// Stage2 Padding
// ============================================================
// Ensure stage2 is properly sized (max 15 sectors = 7680 bytes)
// Actual code will be much smaller, but we leave room for expansion
