#!/bin/bash
#
# scripts/build_image.sh
# @brief Build Cinux disk image with MBR and Stage2
#

set -e  # Exit on error

# ============================================================
# Source Logging Utilities
# ============================================================

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "${SCRIPT_DIR}/log/logging.sh"

# ============================================================
# Path Configuration
# ============================================================

PROJECT_ROOT=$(dirname "$SCRIPT_DIR")

# ============================================================
# Parse Command Line Arguments
# ============================================================

# Usage: build_image.sh [mbr.bin] [stage2.bin] [mini_kernel.bin] [output.img] [big_kernel.bin]
MBR_BIN=${1:-${BUILD_DIR}/boot/mbr.bin}
STAGE2_BIN=${2:-${BUILD_DIR}/boot/stage2.bin}
MINI_BIN=${3:-${BUILD_DIR}/kernel/mini/mini_kernel.bin}
OUTPUT_IMAGE=${4:-${BUILD_DIR}/cinux.img}
BIG_KERNEL_BIN=${5:-""}

# ============================================================
# Ensure Build Directory Exists
# ============================================================

mkdir -p "$(dirname "$OUTPUT_IMAGE")"

# ============================================================
# Validate Input Files
# ============================================================

# Check MBR binary exists
if [ ! -f "$MBR_BIN" ]; then
    log_error "MBR binary not found at $MBR_BIN"
    log_error "Please run 'make' first to build the bootloader."
    exit 1
fi

# Check Stage2 binary exists
if [ ! -f "$STAGE2_BIN" ]; then
    log_error "Stage2 binary not found at $STAGE2_BIN"
    log_error "Please run 'make' first to build the bootloader."
    exit 1
fi

# Check mini kernel bin exists
if [ ! -f "$MINI_BIN" ]; then
    log_error "Mini kernel binary not found at $MINI_BIN"
    log_error "Please run 'make' first to build the mini kernel."
    exit 1
fi

# ============================================================
# Constants
# ============================================================

# Disk layout:
# Sector 0:     MBR (512 bytes)
# Sector 1-15:  Stage2 (up to 15 sectors = 7680 bytes)
# Sector 16+:   Mini kernel (starts at LBA 16, matches MINI_KERNEL_LBA in boot.S)
STAGE2_LBA=1
STAGE2_MAX_SECTORS=15
MINI_KERNEL_LBA=16

# Mini kernel size limit (416KB = 832 sectors)
# Memory layout constraints:
#   - Real mode stack:    0x9000 ~ 0x19000 (SS=0x0900, SP=0xFFFE)
#   - Kernel load area:   0x20000 ~ 0x88000 (must avoid real mode stack)
#   - Protected mode stack: 0x90000 (stage2.S line 168, ESP=0x90000)
#   - Safety gap:         32KB before protected mode stack
# Therefore: max kernel size = 0x88000 - 0x20000 = 0x68000 = 416KB = 832 sectors
MINI_KERNEL_MAX_BYTES=$((416 * 1024))  # 425984 bytes
MINI_KERNEL_MAX_SECTORS=$((MINI_KERNEL_MAX_BYTES / 512))  # 832 sectors

# ============================================================
# Get File Sizes
# ============================================================

# Get Stage2 size
STAGE2_SIZE=$(stat -c%s "$STAGE2_BIN" 2>/dev/null || stat -f%z "$STAGE2_BIN")
STAGE2_SECTORS=$(( (STAGE2_SIZE + 511) / 512 ))

# Get mini kernel size
MINI_SIZE=$(stat -c%s "$MINI_BIN" 2>/dev/null || stat -f%z "$MINI_BIN")
MINI_SECTORS=$(( (MINI_SIZE + 511) / 512 ))

# ============================================================
# Print Component Sizes
# ============================================================

echo ""
echo "=========================================="
echo "  Component Size Summary"
echo "=========================================="
printf "  %-20s %8s bytes  %4s sectors\n" "MBR:" "512" "1"
printf "  %-20s %8d bytes  %4d sectors\n" "Stage2:" "$STAGE2_SIZE" "$STAGE2_SECTORS"
printf "  %-20s %8d bytes  %4d sectors\n" "Mini Kernel:" "$MINI_SIZE" "$MINI_SECTORS"

# Big kernel info (optional)
BIG_KERNEL_LBA=848
BIG_KERNEL_SIZE=0
BIG_KERNEL_SECTORS=0
if [ -n "$BIG_KERNEL_BIN" ] && [ -f "$BIG_KERNEL_BIN" ]; then
    BIG_KERNEL_SIZE=$(stat -c%s "$BIG_KERNEL_BIN" 2>/dev/null || stat -f%z "$BIG_KERNEL_BIN")
    BIG_KERNEL_SECTORS=$(( (BIG_KERNEL_SIZE + 511) / 512 ))
    printf "  %-20s %8d bytes  %4d sectors\n" "Big Kernel:" "$BIG_KERNEL_SIZE" "$BIG_KERNEL_SECTORS"
fi

echo "=========================================="
TOTAL_SIZE=$((512 + STAGE2_SIZE + MINI_SIZE + BIG_KERNEL_SIZE))
TOTAL_SECTORS=$((1 + STAGE2_SECTORS + MINI_SECTORS + BIG_KERNEL_SECTORS))
printf "  %-20s %8d bytes  %4d sectors\n" "Total:" "$TOTAL_SIZE" "$TOTAL_SECTORS"
echo "=========================================="
echo ""

# ============================================================
# Validate Size Limits
# ============================================================

# Validate Stage2 size
if [ $STAGE2_SECTORS -gt $STAGE2_MAX_SECTORS ]; then
    log_error "Stage2 too large!"
    log_error "       Actual:   $STAGE2_SIZE bytes ($STAGE2_SECTORS sectors)"
    log_error "       Maximum:  $((STAGE2_MAX_SECTORS * 512)) bytes ($STAGE2_MAX_SECTORS sectors)"
    exit 1
fi

# Validate mini kernel size
if [ $MINI_SIZE -gt $MINI_KERNEL_MAX_BYTES ]; then
    log_error "Mini kernel too large!"
    log_error "       Actual:   $MINI_SIZE bytes ($MINI_SECTORS sectors)"
    log_error "       Maximum:  $MINI_KERNEL_MAX_BYTES bytes ($MINI_KERNEL_MAX_SECTORS sectors, 416KB)"
    echo ""
    log_error "Memory layout constraints:"
    log_error "  - Real mode stack:       0x9000 ~ 0x19000"
    log_error "  - Kernel load area:      0x20000 ~ 0x88000 (416KB max)"
    log_error "  - Protected mode stack:  0x90000"
    log_error "  - Safety gap:            32KB before protected mode stack"
    echo ""
    log_error "To fix this:"
    log_error "  1. Reduce mini kernel size (remove unused code/features)"
    log_error "  2. Move protected mode stack to a higher address"
    log_error "  3. Load kernel at a higher address (requires BIOS int13, may need A20 gate)"
    exit 1
fi

log_success "Size validation passed: All components within limits"

# ============================================================
# Create Disk Image
# ============================================================

# Step 1: Create blank image (1MB minimum, expand if big kernel present)
IMAGE_SIZE_MB=1
if [ -n "$BIG_KERNEL_BIN" ] && [ -f "$BIG_KERNEL_BIN" ]; then
    NEEDED_SECTORS=$((BIG_KERNEL_LBA + BIG_KERNEL_SECTORS))
    NEEDED_BYTES=$((NEEDED_SECTORS * 512))
    NEEDED_MB=$(( (NEEDED_BYTES + 1048575) / 1048576 ))
    if [ "$NEEDED_MB" -gt "$IMAGE_SIZE_MB" ]; then
        IMAGE_SIZE_MB=$NEEDED_MB
    fi
fi
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=$IMAGE_SIZE_MB status=none

# Step 2: Write MBR to sector 0
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
log_info "MBR written to sector 0"

# Step 3: Write Stage2 starting at sector 1
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
log_info "Stage2 written to sectors $STAGE2_LBA-$((STAGE2_LBA + STAGE2_SECTORS - 1)) ($STAGE2_SECTORS sectors, $STAGE2_SIZE bytes)"

# Step 4: Write mini kernel starting at sector 16
dd if="$MINI_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$MINI_KERNEL_LBA conv=notrunc status=none
log_info "Mini kernel written to sectors $MINI_KERNEL_LBA-$((MINI_KERNEL_LBA + MINI_SECTORS - 1)) ($MINI_SECTORS sectors, $MINI_SIZE bytes)"

# Step 5: Write big kernel starting at sector 848 (if provided)
if [ -n "$BIG_KERNEL_BIN" ] && [ -f "$BIG_KERNEL_BIN" ]; then
    dd if="$BIG_KERNEL_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$BIG_KERNEL_LBA conv=notrunc status=none
    log_info "Big kernel written to sectors $BIG_KERNEL_LBA-$((BIG_KERNEL_LBA + BIG_KERNEL_SECTORS - 1)) ($BIG_KERNEL_SECTORS sectors, $BIG_KERNEL_SIZE bytes)"
else
    log_info "No big kernel binary provided, skipping big kernel write"
fi

# ============================================================
# Verify Image
# ============================================================

# Verify MBR signature
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    log_success "MBR signature valid: 0xAA55"
else
    log_warn "MBR signature invalid: $SIGNATURE (expected 55aa)"
fi

# ============================================================
# Output Result Information
# ============================================================

SIZE=$(stat -c%s "$OUTPUT_IMAGE" 2>/dev/null || stat -f%z "$OUTPUT_IMAGE")
echo ""
log_success "Disk image built successfully!"
echo "  Path:  $OUTPUT_IMAGE"
echo "  Size:  $SIZE bytes"
echo ""
echo "To run Cinux:"
echo "  make run    # or"
echo "  qemu-system-x86_64 -drive file=$OUTPUT_IMAGE,format=raw"
