/**
 * @file boot/boot_info.h
 * @brief Bootloader-to-kernel handoff structure definition
 *
 * This file defines the BootInfo structure that BOTH the bootloader AND the kernel
 * must use to pass information from the boot process to the kernel.
 *
 * IMPORTANT: This header is included by:
 *   - Bootloader C code (boot/elf_loader.c) - compiled with -m32
 *   - Kernel C++ code (kernel/mini/main.cpp) - compiled with -m64
 *
 * The structure layout and field types MUST be identical between 32-bit and 64-bit
 * compilation to ensure correct data interpretation. All fields use explicitly-sized
 * types (uint32_t, uint64_t) and padding is explicit to avoid ABI differences.
 *
 * Data locations (convention between bootloader and kernel):
 *   - BootInfo structure:        physical 0x7000
 *   - Memory map entries:        physical 0x5000 (E820 buffer)
 *   - VESA framebuffer info:     physical 0x6400
 *   - Mini kernel ELF load:      physical 0x10000 (64KB, real mode loader)
 *   - Mini kernel runtime:       physical 0x200000 (2MB, ELF segments target)
 *   - Big kernel load address:   physical 0x1000000 (16MB)
 *
 * Bootloader fills this structure in protected mode before jumping to kernel.
 */

#ifndef BOOT_BOOT_INFO_H
#define BOOT_BOOT_INFO_H

#include <stdint.h>

// ============================================================
// Memory Map Entry (from E820 BIOS call)
// ============================================================
// Layout matches E820 BIOS returned format:
//   Base:   64-bit physical base address
//   Length: 64-bit region length in bytes
//   Type:   32-bit memory type (1=usable, 2=reserved, etc.)
//   ACPI:   32-bit ACPI attributes (extended E820, usually 0 for old BIOS)

/**
 * @brief Single memory map entry from E820 query
 *
 * Represents one contiguous memory region reported by BIOS E820 call.
 * Type values: 1=usable RAM, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, etc.
 */
typedef struct {
    uint64_t base;          // Physical base address of the region
    uint64_t length;        // Region length in bytes
    uint32_t type;          // Memory type (1=usable, 2=reserved, etc.)
    uint32_t acpi;          // ACPI extended attributes (usually 0)
} __attribute__((packed)) MemoryMapEntry;

// Static assertion: ensure struct size matches E820 format (24 bytes)
// Use _Static_assert for C11, static_assert for C++11
#if defined(__cplusplus)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#else
_Static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#endif

// ============================================================
// Boot Information Structure
// ============================================================
// Placed at physical 0x7000 by bootloader before kernel jump.
// Kernel receives this as first argument (in %rdi per System V AMD64 ABI).

/**
 * @brief Complete boot information passed from bootloader to kernel
 *
 * This structure contains all information collected by the bootloader:
 * - Kernel entry point and load information
 * - Framebuffer details for graphics output
 * - Memory map for physical memory management
 *
 * The bootloader fills this in protected mode (stage2.S), then passes
 * a pointer to it as the first argument when jumping to the kernel.
 */
typedef struct {
    // Kernel information
    uint64_t entry_point;       // Virtual entry point address (high-half kernel address)
    uint64_t kernel_phys_base;  // Physical base address where kernel ELF was loaded (0x10000)
    uint64_t kernel_size;       // Actual ELF file size in bytes

    // Framebuffer information (from VESA BIOS calls, stored at 0x6400)
    uint64_t fb_addr;           // Physical framebuffer base address
    uint32_t fb_width;          // Framebuffer width in pixels
    uint32_t fb_height;         // Framebuffer height in pixels
    uint32_t fb_pitch;          // Bytes per scan line (may be > width * bpp)
    uint32_t fb_bpp;            // Bits per pixel (usually 32)

    // Memory map (from E820 BIOS call, stored at 0x5000)
    uint32_t mmap_count;        // Number of valid entries in mmap[] array
    uint32_t _pad;              // Explicit padding for alignment
    MemoryMapEntry mmap[32];    // Memory map entries (max 32 entries)

} __attribute__((packed)) BootInfo;

// Static assertion: ensure BootInfo layout is predictable
// Size: 4*uint64_t(32) + 6*uint32_t(24) + 32*MemoryMapEntry(768) = 824
#if defined(__cplusplus)
static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#else
_Static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#endif

#endif // BOOT_BOOT_INFO_H
