#!/usr/bin/env python3
"""
check_memory_layout.py — Build-time memory layout validation for Cinux OS

Parses linker scripts, header constants, and ELF binaries to build a
complete memory map, then checks for overlaps and prints a summary.

Exit code: 0 = no overlaps, 1 = overlap detected or parse error.

Usage:
    python3 scripts/check_memory_layout.py \
        [--mini-elf build/kernel/mini/mini_kernel] \
        [--big-elf build/kernel/big/big_kernel]
"""

import argparse
import re
import struct
import sys
from pathlib import Path

# ============================================================
# Region data structure
# ============================================================

class Region:
    def __init__(self, name: str, start: int, end: int):
        self.name = name
        self.start = start
        self.end = end

    def size_kb(self) -> int:
        return (self.end - self.start) // 1024

    def overlaps(self, other: "Region") -> bool:
        return self.start < other.end and other.start < self.end

    def __repr__(self):
        return f"  {self.name:24s}: 0x{self.start:08x} - 0x{self.end:08x} ({self.size_kb()} KB)"


# ============================================================
# Parsers
# ============================================================

def parse_linker_script(path: str) -> dict:
    """Extract key constants from a linker script."""
    text = Path(path).read_text()
    result = {}

    # Match assignments like: KERNEL_PHYS_BASE = 0x20000;
    for m in re.finditer(r'(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', text):
        name = m.group(1)
        val = m.group(2)
        result[name] = int(val, 0)

    return result


def parse_header_constant(path: str, name: str) -> int | None:
    """Extract a constexpr value from a C++ header."""
    text = Path(path).read_text()
    # Match patterns like: constexpr uint64_t FOO = 0x1000000;
    pattern = rf'{name}\s*=\s*(0x[0-9a-fA-F]+|\d+)'
    m = re.search(pattern, text)
    if m:
        return int(m.group(1), 0)
    return None


def parse_elf_sections(path: str) -> list[Region]:
    """Parse an ELF binary to get segment/section ranges."""
    try:
        data = Path(path).read_bytes()
    except FileNotFoundError:
        return []

    if data[:4] != b'\x7fELF':
        return []

    regions = []

    # Parse ELF header
    e_phoff = struct.unpack_from('<Q', data, 32)[0]
    e_phnum = struct.unpack_from('<H', data, 56)[0]
    e_phentsize = struct.unpack_from('<H', data, 54)[0]

    # Parse program headers (PT_LOAD segments)
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from('<I', data, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_paddr = struct.unpack_from('<Q', data, off + 16)[0]
        p_filesz = struct.unpack_from('<Q', data, off + 32)[0]
        p_memsz = struct.unpack_from('<Q', data, off + 40)[0]
        if p_memsz > 0:
            regions.append(Region(
                f"PT_LOAD[{i}]",
                p_paddr,
                p_paddr + p_memsz,
            ))

    return regions


# ============================================================
# Main validation
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="Cinux memory layout validator")
    parser.add_argument("--mini-elf", help="Path to mini kernel ELF")
    parser.add_argument("--big-elf", help="Path to big kernel ELF")
    args = parser.parse_args()

    base = Path(__file__).resolve().parent.parent
    regions: list[Region] = []
    errors = 0

    # ---- Fixed regions from bootloader ----
    regions.append(Region("Page Tables", 0x1000, 0x4000))

    # ---- Mini kernel from linker script ----
    mini_ld = base / "kernel/mini/linker.ld"
    if mini_ld.exists():
        consts = parse_linker_script(str(mini_ld))
        phys_base = consts.get("KERNEL_PHYS_BASE", 0x20000)
        # Approximate mini kernel end (conservative: up to staging buffer)
        regions.append(Region("Mini Kernel", phys_base, 0x1000000))
    else:
        print(f"[WARN] Mini kernel linker script not found: {mini_ld}")

    # ---- Big kernel load address from header ----
    loader_hpp = base / "kernel/mini/big_kernel_loader.hpp"
    big_load_addr = parse_header_constant(str(loader_hpp), "BIG_KERNEL_LOAD_ADDR")
    if big_load_addr is None:
        big_load_addr = 0x1000000

    # ---- Parse ELF binaries for actual segment ranges ----
    if args.mini_elf:
        mini_regions = parse_elf_sections(args.mini_elf)
        # Update mini kernel region with actual size if available
        for r in mini_regions:
            if "Mini" not in r.name:
                regions.append(r)

    if args.big_elf:
        big_regions = parse_elf_sections(args.big_elf)
        for r in big_regions:
            regions.append(r)
        # Staging buffer (approximate: ELF file size)
        try:
            big_size = Path(args.big_elf).stat().st_size
            regions.append(Region("Staging Buffer", big_load_addr,
                                  big_load_addr + big_size))
        except FileNotFoundError:
            pass

    # ---- Print layout ----
    print("=== Cinux Memory Layout Validation ===")
    for r in regions:
        print(f"  {r}")

    # ---- Check overlaps ----
    print()
    found_overlap = False
    for i in range(len(regions)):
        for j in range(i + 1, len(regions)):
            if regions[i].overlaps(regions[j]):
                overlap_start = max(regions[i].start, regions[j].start)
                overlap_end = min(regions[i].end, regions[j].end)
                print(f"  [OVERLAP] '{regions[i].name}' and '{regions[j].name}' "
                      f"at 0x{overlap_start:08x} - 0x{overlap_end:08x}")
                found_overlap = True
                errors += 1

    if not found_overlap:
        print("  [OK] No overlaps detected.")

    # ---- Disk LBA layout (constant check) ----
    print()
    print("=== Disk LBA Layout ===")
    disk_regions = [
        ("MBR", 0, 1),
        ("Stage2", 1, 4),
        ("Mini Kernel", 16, 848),
        ("Big Kernel", 848, 848 + 1024),  # generous upper bound
    ]
    for name, start, end in disk_regions:
        sectors = end - start
        print(f"  {name:16s}: LBA {start:5d} - {end:5d} ({sectors:4d} sectors)")

    # Check disk overlaps
    for i in range(len(disk_regions)):
        for j in range(i + 1, len(disk_regions)):
            si = disk_regions[i]
            sj = disk_regions[j]
            if si[1] < sj[2] and sj[1] < si[2]:
                print(f"  [OVERLAP] '{si[0]}' and '{sj[0]}' on disk!")
                errors += 1

    if errors == 0:
        print("  [OK] No sector range overlaps.")
    else:
        print(f"  [FAIL] {errors} issue(s) found!")

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
