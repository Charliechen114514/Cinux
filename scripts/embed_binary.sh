#!/bin/bash
# embed_binary.sh -- Convert a binary file into a linkable ELF object with stable symbol names
#
# Usage: embed_binary.sh <input> <output> <section> <sym_prefix>
#
# objcopy -I binary derives symbol names from the absolute input path,
# replacing non-alphanumeric characters with underscores.  This script
# inspects the generated object and renames the symbols to a stable prefix.
#
# Example:
#   embed_binary.sh initrd.tar initrd.o .initrd _binary_initrd
#   Produces symbols: _binary_initrd_start, _binary_initrd_end, _binary_initrd_size

set -euo pipefail

INPUT="$1"
OUTPUT="$2"
SECTION="$3"
SYM_PREFIX="$4"

OBJCOPY="${OBJCOPY:-objcopy}"

# Step 1: Convert binary to ELF object
"${OBJCOPY}" \
    -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data="${SECTION}",CONTENTS,ALLOC,LOAD,READONLY,DATA \
    "${INPUT}" "${OUTPUT}"

# Step 2: Extract the auto-generated symbol names and rename them
SYM_START=$(nm "${OUTPUT}" | grep '_start$' | awk '{print $3}')
SYM_END=$(nm "${OUTPUT}" | grep '_end$' | awk '{print $3}')
SYM_SIZE=$(nm "${OUTPUT}" | grep '_size$' | awk '{print $3}')

"${OBJCOPY}" \
    --redefine-sym "${SYM_START}=${SYM_PREFIX}_start" \
    --redefine-sym "${SYM_END}=${SYM_PREFIX}_end" \
    --redefine-sym "${SYM_SIZE}=${SYM_PREFIX}_size" \
    "${OUTPUT}"
