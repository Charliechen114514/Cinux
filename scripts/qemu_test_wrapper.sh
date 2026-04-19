#!/bin/bash
# qemu_test_wrapper.sh — Run QEMU and map isa-debug-exit codes to pass/fail
#
# QEMU's isa-debug-exit device encodes: exit_code = (value << 1) | 1
#   Kernel writes 0 → QEMU exits 1 → test SUCCESS
#   Kernel writes 1 → QEMU exits 3 → test FAILURE
#
# Usage: qemu_test_wrapper.sh <qemu> <args...>

"$@"
rc=$?

if [ "$rc" -eq 1 ]; then
    # Kernel wrote 0 (success) → QEMU exit 1
    exit 0
elif [ "$rc" -eq 3 ]; then
    # Kernel wrote 1 (failure) → QEMU exit 3
    exit 1
else
    echo "QEMU unexpected exit code: $rc"
    exit "$rc"
fi
