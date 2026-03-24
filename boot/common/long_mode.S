/**
 * @file boot/common/long_mode.S
 * @brief x86-64 Long Mode Initialization Functions
 *
 * This file contains functions to transition the CPU from 32-bit protected mode
 * to 64-bit long mode. Responsibilities include:
 *   - Setup temporary page tables at fixed address (0x1000-0x3FFF)
 *   - Enable PAE (Physical Address Extension)
 *   - Set LME (Long Mode Enable) bit in EFER
 *   - Extend GDT with 64-bit code segment descriptor
 *   - Perform far jump to 64-bit code segment
 *   - Verify long mode entry with debugcon output
 *
 * AT&T Syntax: source, destination order
 * Page Table Layout (Fixed at 0x1000):
 *   - PML4 at 0x1000 (4096 bytes)
 *   - PDPT at 0x2000 (4096 bytes)
 *   - PD at 0x3000 (4096 bytes)
 *   - Mapping: Identity only (PML4[0]), 2MB pages for first 8MB
 */

// ============================================================
// Constants
// ============================================================

// Page table physical addresses (fixed layout)
.set PML4_PHYS_ADDR,      0x1000      // PML4 table location
.set PDPT_PHYS_ADDR,      0x2000      // PDPT table location
.set PD_PHYS_ADDR,        0x3000      // Page Directory location

// Page table entry flags
.set PAGE_PRESENT,        0x01        // Present bit
.set PAGE_WRITABLE,       0x02        // Read/Write bit
.set PAGE_LARGE,          0x80        // Page Size bit (2MB pages)

// Page entry with flags: present + writable + large
.set PAGE_FLAGS,          (PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE)

// MSR addresses
.set MSR_EFER,            0xC0000080  // Extended Feature Enable Register

// EFER bits
.set EFER_LME,            0x100         // Long Mode Enable (bit 8)

// Control register bits
.set CR4_PAE,             0x20        // PAE enable (bit 5)
.set CR0_PG,              0x80000000  // Paging enable (bit 31)
.set CR0_PE,              0x01        // Protected mode enable (bit 0)

// GDT selectors - imported from stage2.S
// GDT_NULL64    = 0x00  (Null descriptor)
// GDT_CODE32_PM = 0x08  (32-bit protected mode code selector)
// GDT_DATA32_PM = 0x10  (32-bit protected mode data selector)
// GDT_CODE64    = 0x18  (64-bit code selector)
// GDT_DATA64    = 0x20  (64-bit data selector)

// Debugcon I/O port
.set DEBUGCON_PORT,       0xE9

// Character codes for debug output
.set CHAR_LONG_MODE,      0x4C        // 'L'

// ============================================================
// External Symbols
// ============================================================
.extern long_mode_entry            // 64-bit entry point (defined in stage2.S)
.extern gdt64_ptr                  // 64-bit GDT pointer (defined in stage2.S)

// ============================================================
// Section: .text
// ============================================================
.section .text
.code32

// ============================================================
// Function: setup_page_tables
// Responsibility: Initialize temporary page tables for long mode
// Input: None
// Output: None (modifies memory at 0x1000-0x3FFF)
// Clobbers: %eax, %ecx, %edi
// Description:
//   - Clear PML4, PDPT, and PD pages using rep stosl
//   - Setup PML4[0] -> PDPT with present+writable flags
//   - Setup PDPT[0] -> PD with present+writable flags
//   - Setup PD[0..3] for 2MB pages covering 0-8MB memory
// ============================================================
.global setup_page_tables
setup_page_tables:
    // clear df again
    cld
    // Clear PML4 table at 0x1000
    movl $PML4_PHYS_ADDR, %edi       // [imm->edi] Destination = PML4 address
    xorl %eax, %eax                   // [0->eax] Value to write = 0
    movl $1024, %ecx                  // [imm->ecx] Count = 4096/4 = 1024 dwords
    rep stosl                         // [eax->edi] Zero PML4, count=ecx

    // Clear PDPT table at 0x2000
    movl $PDPT_PHYS_ADDR, %edi       // [imm->edi] Destination = PDPT address
    xorl %eax, %eax                   // [0->eax] Value to write = 0
    movl $1024, %ecx                  // [imm->ecx] Count = 1024 dwords
    rep stosl                         // [eax->edi] Zero PDPT, count=ecx

    // Clear PD table at 0x3000
    movl $PD_PHYS_ADDR, %edi         // [imm->edi] Destination = PD address
    xorl %eax, %eax                   // [0->eax] Value to write = 0
    movl $1024, %ecx                  // [imm->ecx] Count = 1024 dwords
    rep stosl                         // [eax->edi] Zero PD, count=ecx

    // Setup PML4[0] -> PDPT
    movl $PDPT_PHYS_ADDR, %eax       // [imm->eax] PDPT physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PML4_PHYS_ADDR        // [eax->mem] PML4[0]=PDPT|flags

    // Setup PDPT[0] -> PD
    movl $PD_PHYS_ADDR, %eax         // [imm->eax] PD physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PDPT_PHYS_ADDR        // [eax->mem] PDPT[0]=PD|flags

    // Setup PD[0..3] for 2MB pages (identity map 0-8MB)
    movl $PD_PHYS_ADDR, %edi
    movl $4, %ecx
    xorl %eax, %eax

1:
    movl %eax, %edx
    shll $21, %edx
    orl $PAGE_FLAGS, %edx

    movl %edx, (%edi)
    addl $8, %edi

    incl %eax
    loop 1b

    // ============================================================
    // Setup Higher-Half Kernel Mapping (004_boot_load_mini_kernel_C)
    // ============================================================
    // Map 0xFFFFFFFF80000000 -> 0x00000000 using the same PD entries
    //
    // Virtual address: 0xFFFFFFFF80020000
    //   - PML4[511] (bits 47:39 = 0x1FF = 511)
    //   - PDPT[510]  (bits 38:30 = 0x1FE = 510)
    //   - PD[1]      (bits 29:21 = 0x001 = 1)  -> maps to 0x20000 physical
    //
    // This allows the kernel to run at either:
    //   - 0x0000000000020000 (identity mapping)
    //   - 0xFFFFFFFF80020000 (higher-half mapping)
    // Both use the same PD[1] entry, so they map to the same physical pages.

    // PML4[511] -> PDPT (same as PML4[0])
    movl $PDPT_PHYS_ADDR, %eax       // [imm->eax] PDPT physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511] = PDPT|flags

    // PDPT[510] -> PD (same as PDPT[0])
    movl $PD_PHYS_ADDR, %eax         // [imm->eax] PD physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // PDPT[510] = PD|flags

    ret

// ============================================================
// Function: enter_long_mode
// Responsibility: Transition CPU from protected mode to long mode
// Input: None
// Output: Never returns (jumps to long_mode_entry in 64-bit mode)
// Description:
//   - Load page table base address into CR3
//   - Enable PAE by setting CR4 bit 5
//   - Enable Long Mode by setting EFER.LME
//   - Enable paging by setting CR0.PG + CR0.PE
//   - Load extended GDT (with 64-bit descriptor)
//   - Perform far jump to 64-bit code segment
// ============================================================
.global enter_long_mode
enter_long_mode:

    // Load page table base into CR3
    movl $PML4_PHYS_ADDR, %eax       // [imm->eax] PML4 table physical address
    movl %eax, %cr3                  // [eax->cr3] Load page table base

    // Enable PAE (Physical Address Extension)
    movl %cr4, %eax                  // [cr4->eax] Read CR4
    orl $CR4_PAE, %eax                // [imm->eax] Set PAE bit (bit 5)
    movl %eax, %cr4                  // [eax->cr4] Write CR4 with PAE

    // Enable Long Mode (EFER.LME)
    movl $MSR_EFER, %ecx             // [imm->ecx] MSR_EFER address = 0xC0000080
    rdmsr                            // [msr->eax] Read EFER into edx:eax
    orl $EFER_LME, %eax               // [imm->eax] Set LME bit (bit 8)
    wrmsr                            // [eax->msr] Write EFER from edx:eax

    // Load extended GDT with 64-bit descriptors
    lgdt gdt64_ptr                  // [mem->gdtr] Load GDT with 64-bit descriptor
    
    // Enable Paging (and ensure Protected Mode)
    movl %cr0, %eax                  // [cr0->eax] Read CR0
    orl $(CR0_PG | CR0_PE), %eax      // [imm->eax] Set PG+PE bits (0x80000001)
    movl %eax, %cr0                  // [eax->cr0] Write CR0 with paging enabled

    // Far jump to 64-bit code segment
    ljmp $0x18, $long_mode_entry    // [imm->rip] Jump to 64-bit code

    // Should never reach here
    cli
.lm_halt:
    hlt
    jmp .lm_halt

// ============================================================
// Section: .code64
// These symbols are referenced but defined in stage2.S
// ============================================================

// long_mode_entry is defined in stage2.S
// This section is just documentation of the expected interface

/**
 * @struct LongModeGDTDesc
 * @brief 64-bit GDT descriptor format
 *
 * Layout: .quad <base> (64-bit base address)
 *         .word <limit> (16-bit limit)
 */
