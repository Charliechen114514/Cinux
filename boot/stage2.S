/**
 * @file boot/stage2.S
 * @brief Cinux Stage2 Bootloader - VESA and Long Mode Initialization
 *
 * This file is loaded by MBR to 0x8000 and continues the boot process.
 * Responsibilities:
 *   - Print "Stage2 OK" message
 *   - Enable A20 line
 *   - Get VESA controller information
 *   - Get VESA mode information (0x118: 1024x768x32)
 *   - Set VESA video mode with linear framebuffer
 *   - Save framebuffer information to fixed location
 *   - Switch to protected mode
 *   - Setup page tables and enter long mode
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
// External functions from common/boot.S (004_boot_load_mini_kernel_A)
// ============================================================
.extern query_memory_map
.extern load_kernel_from_disk

// ============================================================
// External functions from common/long_mode.S
// ============================================================
.extern setup_page_tables
.extern enter_long_mode

// ============================================================
// Constants from common/serial.S
// ============================================================
.set VBE_FB_INFO_BLOCK,         0x6400
.set STAGE2_STACK_BASEADDR,     0xFFFE

// Framebuffer info layout offsets
.set FB_INFO_PHYS_ADDR,         0       // Physical base address (8 bytes)
.set FB_INFO_PITCH,             8       // Bytes per scan line (4 bytes)
.set FB_INFO_WIDTH,             12      // Width in pixels (2 bytes)

// GDT selectors for long mode
.set GDT_CODE64,                0x18     // 64-bit code selector
.set GDT_DATA64,                0x20     // 64-bit data selector

// Debugcon
.set DEBUGCON_PORT,             0xE9
.set CHAR_LONG_MODE,            0x4C     // 'L' for Long Mode verification

// Kernel LBA (must match boot/common/boot.S)
.set MINI_KERNEL_LBA,           16       // Mini kernel start LBA (sector 16)

// ============================================================
// Function: _start
// Responsibility: Stage2 entry point, called from MBR
// Input: None (execution starts here)
// Output: Never returns (continues to long mode)
// ============================================================
_start:
    cli                             // Disable interrupts during critical setup

    // Reset all segment registers
    movw %cs, %ax                 // Load CS into AX
    movw %ax, %ds                 // Set DS = CS
    movw %ax, %es                 // Set ES = CS
    movw %ax, %fs                 // Set FS = CS
    movw %ax, %gs                 // Set GS = CS

    movw $0x900, %ax             // 0x900 << 4 = 0x9000
    movw %ax, %ss                // Set SS to 0x9000

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

    // ============================================================
    // 004_boot_load_mini_kernel_A: Real mode 内完成
    // ============================================================

    call query_memory_map           // [→0x5000] E820 memory map

    call load_kernel_from_disk      // [→0x20000] Load mini kernel ELF (416KB, 832 sectors) to 0x88000, leaving 32KB gap before protected mode stack at 0x90000    

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

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Entry point is fixed at 0xFFFFFFFF80020000 (high-half kernel).
    // Nothing to do in protected mode, proceed to long mode.

    // ============================================================
    // Transition to Long Mode
    // ============================================================

    // Setup page tables for long mode
    call setup_page_tables          // [->mem] Setup page tables at 0x1000-0x3FFF

    // Enter long mode
    call enter_long_mode            // [->rip] Jump to 64-bit mode

    // Should never reach here
    cli
.pm_halt:
    hlt
    jmp .pm_halt

// ============================================================
// Long Mode Entry Point
// ============================================================
.code64                          // Now in 64-bit long mode
.global long_mode_entry
long_mode_entry:
    // GDT is already loaded by enter_long_mode before far jump

    // Set up data segment registers for 64-bit mode
    movw $GDT_DATA64, %ax           // [imm->ax] Load 64-bit data selector
    movw %ax, %ds                   // [ax->ds] Set DS to 64-bit data selector
    movw %ax, %es                   // [ax->es] Set ES
    movw %ax, %fs                   // [ax->fs] Set FS
    movw %ax, %gs                   // [ax->gs] Set GS
    movw %ax, %ss                   // [ax->ss] Set SS

    // Set up 64-bit stack
    movabsq $0x90000, %rsp          // [imm->rsp] Set 64-bit stack pointer

    // Verify long mode entry with debugcon output
    movb $CHAR_LONG_MODE, %al       // [imm->al] Load 'L' (0x4C)
    outb %al, $DEBUGCON_PORT        // [al->port] Output 'L' to debugcon

    // ============================================================
    // 004_boot_load_mini_kernel_C: Fill BootInfo and Jump to Mini Kernel
    // ============================================================

    // BootInfo is placed at physical 0x7000
    // Layout matches boot/boot_info.h:
    //   entry_point       (8 bytes) offset 0
    //   kernel_phys_base  (8 bytes) offset 8
    //   kernel_size       (8 bytes) offset 16
    //   fb_addr           (8 bytes) offset 24
    //   fb_width          (4 bytes) offset 32
    //   fb_height         (4 bytes) offset 36
    //   fb_pitch          (4 bytes) offset 40
    //   fb_bpp            (4 bytes) offset 44
    //   mmap_count        (4 bytes) offset 48
    //   _pad              (4 bytes) offset 52
    //   mmap[32]          (768 bytes) offset 56

    movq $0x7000, %rdi              // BootInfo destination address

    // 1. entry_point: higher-half kernel virtual address
    //    VMA = 0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000
    movq $0xFFFFFFFF80020000, %rax
    movq %rax, (%rdi)               // [0x7000] = entry_point

    // 2. kernel_phys_base: physical load address
    movq $0x20000, %rax
    movq %rax, 8(%rdi)              // [0x7008] = kernel_phys_base

    // 3. kernel_size: actual size (will be read from ELF later)
    movq $0x68000, %rax             // 416KB = 0x68000 bytes (max)
    movq %rax, 16(%rdi)             // [0x7010] = kernel_size

    // 4. Framebuffer info: copy from 0x6400
    //    0x6400: fb_addr (8B)
    //    0x6408: fb_pitch (4B)
    //    0x640C: fb_width (2B)
    //    0x640E: fb_height (2B)
    movq $0x6400, %rsi              // Source: VESA framebuffer info
    movq (%rsi), %rax               // Read fb_addr
    movq %rax, 24(%rdi)             // [0x7018] = fb_addr

    movl 8(%rsi), %eax              // Read fb_pitch
    movl %eax, 40(%rdi)             // [0x7028] = fb_pitch

    movzwq 12(%rsi), %rax           // Read fb_width (zero-extend 16-bit)
    movl %eax, 32(%rdi)             // [0x7020] = fb_width

    movzwq 14(%rsi), %rax           // Read fb_height (zero-extend 16-bit)
    movl %eax, 36(%rdi)             // [0x7024] = fb_height

    // fb_bpp: assume 32 (standard for VESA mode 0x118)
    movl $32, %eax
    movl %eax, 44(%rdi)             // [0x702C] = fb_bpp

    // 5. Memory map: copy from 0x5000
    //    0x5000: mmap_count (4B) + mmap entries (24B each)
    movq $0x5000, %rsi              // Source: E820 memory map
    movl (%rsi), %eax               // Read mmap_count
    movl %eax, 48(%rdi)             // [0x7030] = mmap_count

    // Copy mmap entries (max 32 entries * 24 bytes = 768 bytes)
    movq $56, %rdx                  // Destination offset in BootInfo
    movq $4, %rcx                   // Source offset (skip count)
    movq $768, %r8                  // Bytes to copy
1:
    movb (%rsi, %rcx), %al
    movb %al, (%rdi, %rdx)
    incq %rcx
    incq %rdx
    decq %r8
    jnz 1b

    // ============================================================
    // Jump to Mini Kernel
    // ============================================================

    // Test 1: Verify identity mapping to kernel
    movq $0x20000, %rax             // Identity address
    movb (%rax), %al                // Read first byte (should be 0xFA = 'cli')
    outb %al, $DEBUGCON_PORT        // Output: should be 0xFA

    // Test 2: Verify higher-half mapping
    movq $0xFFFFFFFF80020000, %rax  // Higher-half address
    movb (%rax), %al                // Read first byte
    outb %al, $DEBUGCON_PORT        // Output: should be 0xFA if mapping works

    // Test 3: Jump to kernel using higher-half mapping
    movb $0x48, %al                 // 'H' for Higher-half jump
    outb %al, $DEBUGCON_PORT

    movq $0x7000, %rdi              // First argument: BootInfo*
    movq $0xFFFFFFFF80020000, %rax  // Entry point: _start (higher-half)
    jmp *%rax

    // Should never reach here
    cli
.lm_halt:
    hlt
    jmp .lm_halt

// ============================================================
// Error Handlers
// ============================================================
a20_failed:
    jmp panic

// ============================================================
// GDT (Global Descriptor Table) - in .gdt section
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

// 64-bit code descriptor (L=1, D=0)
// Value: 0x00AF9A000000FFFF
// Format: limit(0-15)=FFFF, base(0-23)=00, base(24-31)=00
//         access=9A (present, DPL=0, code, executable, readable, 64-bit)
//         flags=AF (64-bit, 4KB gran, limit=0xF)
gdt_code64:
    .quad 0x00AF9A000000FFFF       // 64-bit code descriptor (L=1, D=0)

// 64-bit data descriptor
// Value: 0x008F92000000FFFF
// Format: limit=FFFF, base=0
//         access=92 (present, DPL=0, data, writable)
//         flags=8F (32-bit, 4KB gran, limit=0xF)
gdt_data64:
    .quad 0x008F92000000FFFF       // 64-bit data descriptor

gdt_end:

// 32-bit GDT pointer for protected mode entry
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Linear address of GDT base (32-bit)

// 64-bit GDT pointer for long mode reload
// Note: Use .long + .long instead of .quad to avoid 64-bit relocation
// in 32-bit ELF. GDT is at low address so upper 32 bits are zero.
.global gdt64_ptr
gdt64_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Lower 32 bits of GDT address
    .long 0                        // Upper 32 bits (zero, GDT is in low memory)

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
