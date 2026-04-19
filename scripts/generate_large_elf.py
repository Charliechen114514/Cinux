#!/usr/bin/env python3
"""
generate_large_elf.py — Generate a synthetic large ELF64 binary for stress testing

Creates a valid ELF64 binary with one PT_LOAD segment of configurable size,
filled with a known data pattern for integrity verification.

The pattern used: byte = (offset >> 12) & 0xFF (changes every 4KB).
A CRC32 checksum is appended after the ELF data.

Usage:
    python3 scripts/generate_large_elf.py --size 1073741824 --output build/stress_kernel.elf
"""

import argparse
import struct
import zlib
import sys
from pathlib import Path


# ELF64 constants
ELFMAG = b'\x7fELF'
ELFCLASS64 = 2
ELFDATA2LSB = 1
EV_CURRENT = 1
ELFOSABI_NONE = 0
ET_EXEC = 2
EM_X86_64 = 62

# PT_LOAD type
PT_LOAD = 1

# Segment permissions
PF_R = 4
PF_W = 2
PF_X = 1

# Layout constants
ELF_HEADER_SIZE = 64
PHDR_SIZE = 56
PAGE_ALIGN = 0x1000

# Big kernel physical load address (must match KERNEL_LMA in kernel/linker.ld)
BIG_KERNEL_PADDR = 0x1000000
HIGHER_HALF_BASE = 0xFFFFFFFF80000000


def build_elf_header(entry: int, phoff: int, phnum: int) -> bytes:
    """Build a 64-byte ELF64 header."""
    # Build the 16-byte e_ident block
    e_ident = (ELFMAG
               + bytes([ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE])
               + b'\x00' * 8)
    return struct.pack('<16sHHIQQQIHHHHHH',
        e_ident,                    # e_ident[0:16]  16-byte identification
        ET_EXEC,                    # e_type
        EM_X86_64,                  # e_machine
        EV_CURRENT,                 # e_version
        entry,                      # e_entry
        phoff,                      # e_phoff
        0,                          # e_shoff (no section headers)
        0,                          # e_flags
        ELF_HEADER_SIZE,            # e_ehsize
        PHDR_SIZE,                  # e_phentsize
        phnum,                      # e_phnum
        0,                          # e_shentsize
        0,                          # e_shnum
        0,                          # e_shstrndx
    )


def build_phdr(p_type: int, p_flags: int, p_offset: int, p_vaddr: int,
               p_paddr: int, p_filesz: int, p_memsz: int, p_align: int) -> bytes:
    """Build a 56-byte ELF64 program header."""
    return struct.pack('<IIQQQQQQ',
        p_type,
        p_flags,
        p_offset,
        p_vaddr,
        p_paddr,
        p_filesz,
        p_memsz,
        p_align,
    )


def generate_pattern(offset: int, size: int) -> bytes:
    """Generate data with the known pattern: byte = (offset >> 12) & 0xFF."""
    data = bytearray(size)
    for i in range(size):
        data[i] = ((offset + i) >> 12) & 0xFF
    return bytes(data)


# x86 'cli' instruction — expected at kernel entry point by test_big_kernel_first_insn
ENTRY_BYTE_CLI = 0xFA


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic large ELF")
    parser.add_argument("--size", type=int, default=1024 * 1024 * 1024,
                        help="Approximate total file size in bytes (default: 1GB)")
    parser.add_argument("--output", default="build/stress_kernel.elf",
                        help="Output file path")
    args = parser.parse_args()

    target_size = args.size
    output_path = Path(args.output)

    # Layout:
    #   [0x0000 - 0x003F]  ELF header (64 bytes)
    #   [0x0040 - 0x0077]  Program header (56 bytes)
    #   [0x1000 - ...]     Segment data (page-aligned)
    #
    # Segment data size = target_size - 0x1000 (account for header overhead)
    segment_data_size = max(target_size - PAGE_ALIGN, 4096)
    total_elf_size = PAGE_ALIGN + segment_data_size  # ELF data end (before CRC)

    entry = HIGHER_HALF_BASE + BIG_KERNEL_PADDR
    p_vaddr = entry
    p_paddr = BIG_KERNEL_PADDR
    p_offset = PAGE_ALIGN  # segment data starts at page-aligned offset

    # Build headers
    ehdr = build_elf_header(entry, ELF_HEADER_SIZE, 1)
    phdr = build_phdr(
        PT_LOAD,
        PF_R | PF_W | PF_X,
        p_offset,
        p_vaddr,
        p_paddr,
        segment_data_size,  # filesz
        segment_data_size,  # memsz
        PAGE_ALIGN,
    )

    print(f"Generating stress test ELF:")
    print(f"  Segment size: {segment_data_size / (1024*1024):.1f} MB")
    print(f"  Total file:   {total_elf_size / (1024*1024):.1f} MB")
    print(f"  p_paddr:      0x{p_paddr:x}")
    print(f"  Entry point:  0x{entry:x}")

    # Write the file
    output_path.parent.mkdir(parents=True, exist_ok=True)
    crc = 0

    with open(output_path, 'wb') as f:
        # Write ELF header + program header
        f.write(ehdr)
        f.write(phdr)

        # Pad to page boundary
        pad_size = PAGE_ALIGN - ELF_HEADER_SIZE - PHDR_SIZE
        f.write(b'\x00' * pad_size)

        # CRC over headers + padding
        crc = zlib.crc32(ehdr, crc)
        crc = zlib.crc32(phdr, crc)
        crc = zlib.crc32(b'\x00' * pad_size, crc)

        # Write segment data in 1MB chunks with pattern
        CHUNK = 1024 * 1024
        written = 0
        first_chunk = True
        while written < segment_data_size:
            chunk_size = min(CHUNK, segment_data_size - written)
            chunk = generate_pattern(written, chunk_size)
            # Place 'cli' (0xFA) at the entry point (offset 0 of segment)
            # so test_big_kernel_first_insn passes with the synthetic ELF.
            if first_chunk:
                chunk = bytes([ENTRY_BYTE_CLI]) + chunk[1:]
                first_chunk = False
            f.write(chunk)
            crc = zlib.crc32(chunk, crc)
            written += chunk_size
            if written % (100 * CHUNK) == 0:
                print(f"  Progress: {written / (1024*1024):.0f} MB / "
                      f"{segment_data_size / (1024*1024):.0f} MB")

    # Append CRC32 (4 bytes, little-endian)
    crc_final = crc & 0xFFFFFFFF
    with open(output_path, 'ab') as f:
        f.write(struct.pack('<I', crc_final))

    actual_size = output_path.stat().st_size
    print(f"  Written:      {actual_size / (1024*1024):.1f} MB")
    print(f"  CRC32:        0x{crc_final:08x}")
    print(f"  Output:       {output_path}")
    print(f"Done.")


if __name__ == "__main__":
    main()
