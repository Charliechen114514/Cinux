#!/bin/bash
# Create a 1 MB test disk image with MBR boot signature (0x55AA at offset 510-511)

set -e
OUTPUT="$1"

dd if=/dev/zero of="$OUTPUT" bs=1M count=1 2>/dev/null
printf '\x55' | dd of="$OUTPUT" bs=1 seek=510 conv=notrunc 2>/dev/null
printf '\xaa' | dd of="$OUTPUT" bs=1 seek=511 conv=notrunc 2>/dev/null
