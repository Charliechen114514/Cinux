#!/usr/bin/env python3
"""
append_crc32.py - Append CRC32 checksum to a binary file.

Usage: python3 append_crc32.py <input_file>

Computes CRC32 of the file content, appends it as 4 bytes (little-endian),
then pads the file to 512-byte alignment.
"""

import binascii
import struct
import sys
import os

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <binary_file>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    data = open(path, "rb").read()
    crc = binascii.crc32(data) & 0xFFFFFFFF

    with open(path, "ab") as f:
        f.write(struct.pack("<I", crc))

    sz = os.path.getsize(path)
    pad = (512 - (sz % 512)) % 512
    if pad:
        with open(path, "ab") as f:
            f.write(b"\x00" * pad)

    final_sz = os.path.getsize(path)
    print(f"Big kernel CRC32: 0x{crc:08x}, total: {final_sz} bytes")

if __name__ == "__main__":
    main()
