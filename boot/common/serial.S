/**
 * @file boot/common/serial.S
 * @brief Common serial output functions for bootloader
 *
 * This file provides print_string function that can be used by both MBR and stage2.
 * Uses BIOS INT 0x10 AH=0x0E for screen output in real mode.
 *
 * AT&T Syntax: source, destination order
 * Registers: % prefix, Immediates: $ prefix
 */

.section .text
.code16                        // Generate 16-bit code (works with --32)

// ============================================================
// VBE/VESA Structure Offsets
// ============================================================
// VbeInfoBlock (512 bytes at 0x6000)
.set VBE_VBE_INFO_BLOCK,       0x6000
.set VBE_MODE_INFO_BLOCK,      0x6200
.set VBE_FB_INFO_BLOCK,        0x6400

// ModeInfoBlock offsets (VBE 2.0)
.set MODE_PHYS_BASE_PTR,       0x28   // PhysBasePtr (4 bytes)
.set MODE_BYTES_PER_SCAN_LINE, 0x10   // BytesPerScanLine (2 bytes)
.set MODE_X_RESOLUTION,        0x12   // XResolution (2 bytes)
.set MODE_Y_RESOLUTION,        0x14   // YResolution (2 bytes)
.set MODE_BITS_PER_PIXEL,      0x19   // BitsPerPixel (1 byte)

// VESA framebuffer info layout (16 bytes at 0x6400)
// Layout: phys_addr(8B) + pitch(4B) + width(2B) + height(2B) [+ bpp optional]
.set FB_INFO_PHYS_ADDR,        0       // Physical base address (8 bytes)
.set FB_INFO_PITCH,            8       // Bytes per scan line (4 bytes)
.set FB_INFO_WIDTH,            12      // Width in pixels (2 bytes)
.set FB_INFO_HEIGHT,           14      // Height in pixels (2 bytes)
// .set FB_INFO_BPP,             16      // Bits per pixel (2 bytes) - optional

// Target video mode
.set VESA_TARGET_MODE,         0x4118  // 0x118 with bit14 set (linear framebuffer)

// ============================================================
// Function: print_string
// Responsibility: Print null-terminated string to screen via BIOS
// Input: %si = pointer to null-terminated string
// Output: None
// Modifies: %ax, %bx, %si
// ============================================================
.global print_string
.type print_string, @function
print_string:
    pushw %ax
    pushw %bx
    pushw %si
    cld

.loop:
    lodsb
    test %al, %al
    jz .done

    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    jmp .loop

.done:
    popw %si
    popw %bx
    popw %ax
    ret

// ============================================================
// Function: panic
// Responsibility: Print error message and halt the system
// Input: %si = pointer to error message string
// Output: Never returns (halts CPU)
// Modifies: %ax, %bx, %si
// ============================================================
.global panic
.type panic, @function
panic:
    call print_string
    cli
.halt_loop:
    hlt
    jmp .halt_loop

// ============================================================
// Function: enable_a20
// Responsibility: Enable A20 line using BIOS INT 0x15
// Input: None
// Output: %al = 0x01 on success, 0x00 on failure
// Modifies: %ax
// ============================================================
.global enable_a20
.type   enable_a20, @function
.set    A20_COMMAND, 0x2401
enable_a20:
    pushw %ax
    pushw %ds
    pushw %es

    movw $0x2401, %ax               // AH=0x24, AL=0x01 (enable A20)
    int $0x15

    jc .a20_failed

    popw %es
    popw %ds
    popw %ax

    ret

.a20_failed:
    popw %es
    popw %ds
    popw %ax
    movw $(a20_error_msg), %si
    jmp panic

// ============================================================
// Function: vesa_get_controller_info
// Responsibility: Get VBE controller information via INT 0x10 AX=0x4F00
// Input: None
// Output: %al = 0x4F on success, %ah = return code
// Modifies: %ax, %bx, %es, %di
// ============================================================
.global vesa_get_controller_info
.type vesa_get_controller_info, @function
vesa_get_controller_info:
    pushw %ax
    pushw %es
    pushw %di

    // [→ax] Calculate segment (0x6000 / 16)
    movw $VBE_VBE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di
    // signature: "VBE2" required for VBE 2.0+
    movw $0x4256, %es:(%di)     // 'VB'
    movw $0x3245, %es:2(%di)    // 'E2'
    // [→ax] VBE get controller info
    movw $0x4F00, %ax
    int $0x10


    // check status
    cmpb $0x4F, %al
    jne .vesa_ctrl_failed

    cmpb $0, %ah
    jne .vesa_ctrl_failed

    popw %di
    popw %es
    pop %ax
    ret

.vesa_ctrl_failed:
    popw %di
    popw %es
    popw %ax
    movw $(vesa_ctrl_error_msg), %si    // [→si] Load error message
    jmp panic                           // [→never] Call panic


// ============================================================
// Function: vesa_get_mode_info
// Responsibility: Get VBE mode information for mode 0x118
// Input: None
// Output: %al = 0x4F on success, %ah = return code
// Modifies: %ax, %bx, %cx, %es, %di
// ============================================================
.global vesa_get_mode_info
.type vesa_get_mode_info, @function
vesa_get_mode_info:
    pushw %ax
    pushw %cx
    pushw %es
    pushw %di

    movw $VBE_MODE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di

    movw $0x4F01, %ax               // [→ax] VBE get mode info
    movw $0x0118, %cx               // [→cx] Mode number (0x118)
    int $0x10                       // [→bios] Call video interrupt

    // done, check the sessions
    cmpb $0x4F, %al
    jne .vesa_mode_failed

    cmpb $0x0, %ah
    jne .vesa_mode_failed

    popw %di
    popw %es
    popw %cx
    popw %ax
    ret

.vesa_mode_failed:
    popw %di
    popw %es
    popw %cx
    popw %ax
    movw $(vesa_mode_error_msg), %si
    jmp panic

// ============================================================
// Function: vesa_set_mode
// Responsibility: Set VESA video mode with linear framebuffer
// Input: None
// Output: %al = 0x4F on success, %ah = return code
// Modifies: %ax, %bx
// ============================================================
.global vesa_set_mode
.type vesa_set_mode, @function
vesa_set_mode:
    // TODO: Step 1: Call BIOS INT 0x10 AX=0x4F02 BX=0x4118
    // Note: bit 14 (0x4000) enables linear framebuffer
    movw $0x4F02, %ax               // [→ax] VBE set mode function
    movw $VESA_TARGET_MODE, %bx     // [→bx] Mode with linear FB
    int $0x10                       // [→bios] Call video interrupt

    // Got Still LOL
    cmpb $0x4F, %al
    jne .vesa_set_failed

    cmpb $0, %ah
    jne .vesa_set_failed

    ret

.vesa_set_failed:
    movw $(vesa_set_error_msg), %si     // [→si] Load error message
    jmp panic                           // [→never] Call panic

// ============================================================
// Function: vesa_save_framebuffer_info
// Responsibility: Save framebuffer info to fixed location (0x6400)
// Input: None
// Output: None
// Modifies: %ax, %bx, %di, %es, %gs
// ============================================================
.global vesa_save_framebuffer_info
.type vesa_save_framebuffer_info, @function
vesa_save_framebuffer_info:
    pushw %ax
    pushw %bx
    pushw %di
    pushw %es
    pushw %gs

    // ES -> VBE_MODE_INFO_BLOCK (0x6200)
    movw $VBE_MODE_INFO_BLOCK >> 4, %ax
    movw %ax, %es
    xorw %di, %di                     // DI = 0

    // GS -> VBE_FB_INFO_BLOCK (0x6400)
    movw $VBE_FB_INFO_BLOCK >> 4, %bx
    movw %bx, %gs

    // CRITICAL FIX: Use segment override prefixes!
    // Read from ES:0x28 (ModeInfoBlock.PhysBasePtr)
    movl %es:MODE_PHYS_BASE_PTR(%di), %eax
    // Write to GS:0x00 (FBInfo.phys_addr low 4 bytes)
    movl %eax, %gs:FB_INFO_PHYS_ADDR(%di)

    // Read pitch from ES:0x10, write to GS:0x08
    movw %es:MODE_BYTES_PER_SCAN_LINE(%di), %ax
    movw %ax, %gs:FB_INFO_PITCH(%di)

    // Read width from ES:0x12, write to GS:0x0C
    movw %es:MODE_X_RESOLUTION(%di), %ax
    movw %ax, %gs:FB_INFO_WIDTH(%di)

    // Read height from ES:0x14, write to GS:0x0E
    movw %es:MODE_Y_RESOLUTION(%di), %ax
    movw %ax, %gs:FB_INFO_HEIGHT(%di)

    popw %gs
    popw %es
    popw %di
    popw %bx
    popw %ax
    ret

.section .data
a20_error_msg:
    .asciz "A20: Failed to enable!\r\n"

vesa_ctrl_error_msg:
    .asciz "VESA: Controller info failed!\r\n"

vesa_mode_error_msg:
    .asciz "VESA: Mode 0x118 not available!\r\n"

vesa_set_error_msg:
    .asciz "VESA: Failed to set mode!\r\n"