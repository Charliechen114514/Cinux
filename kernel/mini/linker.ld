/* ==============================================================
 * Cinux Mini Kernel - Minimal Linker Script
 * ==============================================================

 * Virtual address (VMA):  0xFFFFFFFF80000000 (higher-half kernel)
 * Physical load (LMA):     0x20000 (128KB)
 * Flat binary: objcopy -O binary removes all ELF headers
 *
 * Memory layout:
 *   - Identity mapping:   0x0000000000020000 -> 0x20000 (physical)
 *   - Higher-half mapping: 0xFFFFFFFF80020000 -> 0x20000 (physical)
 *   Both mappings point to the same physical pages via PD[1].
 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* Kernel physical load address and virtual base offset */
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Higher-half kernel virtual base address with physical offset */
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    /* All read-only sections combined - use wildcard to catch all .text subsections */
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    /* All read-write sections combined */
    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {
        *(.data .data.*)
        __init_array_start = .;
        *(.init_array .init_array.*)
        __init_array_end = .;
    }

    /* .bss section: no LMA needed (cleared at runtime) */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Kernel end markers */
    __mini_kernel_end = .;

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
