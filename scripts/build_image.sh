#!/bin/bash
#
# scripts/build_image.sh
# @brief 构建并生成 Cinux 磁盘镜像
#

set -e  # 遇到错误立即退出

# ============================================================
# 路径配置
# ============================================================

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
BUILD_DIR=${PROJECT_ROOT}/build
OUTPUT_IMAGE=${1:-${BUILD_DIR}/cinux.img}

# ============================================================
# 确保构建目录存在
# ============================================================

mkdir -p "$BUILD_DIR"

# ============================================================
# 源文件路径配置
# ============================================================

MBR_BIN=${BUILD_DIR}/boot/mbr.bin

# 检查 MBR 是否存在
if [ ! -f "$MBR_BIN" ]; then
    echo "Error: MBR binary not found at $MBR_BIN"
    echo "Please run 'make' first to build the bootloader."
    exit 1
fi

# ============================================================
# 创建磁盘镜像
# ============================================================

# 步骤 1：创建空白镜像（1MB）
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# 步骤 2：写入 MBR
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none

# ============================================================
# 验证镜像
# ============================================================

# 验证 MBR 签名
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    echo "MBR signature valid: 0xAA55"
else
    echo "Warning: MBR signature invalid: $SIGNATURE (expected 55aa)"
fi

# ============================================================
# 输出结果信息
# ============================================================

SIZE=$(stat -c%s "$OUTPUT_IMAGE" 2>/dev/null || stat -f%z "$OUTPUT_IMAGE")
echo "Disk image built successfully!"
echo "  Path: $OUTPUT_IMAGE"
echo "  Size: $SIZE bytes"
echo ""
echo "To run Cinux:"
echo "  make run    # or"
echo "  qemu-system-x86_64 -drive file=$OUTPUT_IMAGE,format=raw -serial stdio"