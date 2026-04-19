/* ==============================================================
 * Cinux Big Kernel - Linker Script
 * ==============================================================

 * Virtual address (VMA):  0xFFFFFFFF80000000 (higher-half kernel)
 * Physical load (LMA):    0x1000000 (16MB, loaded by mini kernel ELF loader)
 * Entry: _start (must be the very first byte of .text)
 *
 * The mini kernel loads this ELF binary from disk, parses PT_LOAD
 * segments, and jumps to the entry point.  Because we are a proper
 * ELF binary (not a flat blob), p_paddr / p_vaddr in the program
 * headers determine the load addresses -- the linker script only
 * needs to lay out sections relative to the VMA base.
 *
 * Memory layout:
 *   .text.start  -> first page, contains _start
 *   .text        -> rest of code
 *   .rodata      -> read-only data
 *   .data        -> writable initialised data
 *   .init_array  -> global constructor pointers
 *   .bss         -> zero-initialised data
 *   stack        -> 16 KB guard + 16 KB stack
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* ==============================================================
 * Address constants
 * ============================================================== */
KERNEL_VMA   = 0xFFFFFFFF80000000;   /* higher-half virtual base */
KERNEL_LMA   = 0x1000000;            /* physical load address (16 MB) */

SECTIONS
{
    /* ----------------------------------------------------------
     * Virtual start = VMA base + physical offset
     * This way the linker produces virtual addresses in the
     * higher-half range while AT() keeps LMA physical.
     * ---------------------------------------------------------- */
    . = KERNEL_VMA + KERNEL_LMA;

    /* ----------------------------------------------------------
     * .text -- code + read-only data (RX)
     * ---------------------------------------------------------- */
    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text.start)         /* _start MUST be first */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    /* ----------------------------------------------------------
     * .data -- writable initialised data (RW)
     * ---------------------------------------------------------- */
    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        *(.data .data.*)
    }

    /* ----------------------------------------------------------
     * .init_array -- global constructors
     * ---------------------------------------------------------- */
    .init_array : AT(ADDR(.init_array) - KERNEL_VMA) ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end   = .;
    }

    /* ----------------------------------------------------------
     * .bss -- zero-initialised data (no file content)
     * ---------------------------------------------------------- */
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* ----------------------------------------------------------
     * End markers (used by PMM later)
     * ---------------------------------------------------------- */
    __kernel_end = .;
    PROVIDE(__kernel_size = __kernel_end - (KERNEL_VMA + KERNEL_LMA));

    /* ----------------------------------------------------------
     * Stack (16 KB, placed after BSS)
     * ---------------------------------------------------------- */
    .stack (NOLOAD) : ALIGN(4096) {
        . = . + 0x4000;          /* 16 KB stack */
        __kernel_stack_top = .;
    }

    /* ----------------------------------------------------------
     * Discarded sections
     * ---------------------------------------------------------- */
    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
