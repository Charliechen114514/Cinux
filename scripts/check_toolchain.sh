#!/bin/bash
#
# scripts/check_toolchain.sh
# @brief 验证 Cinux 开发所需的工具链是否已安装
#
# 检查以下命令是否可用：
#   - gcc-multilib (i686-linux-gnu-gcc)
#   - g++-multilib (i686-linux-gnu-g++)
#   - binutils (as, ld, objcopy)
#   - qemu-system-x86
#   - cmake
#

source "scripts/log/logging.sh"
log_info "Checking Tool Chains For the Cinus..."

# TODO: 实现工具链检查逻辑
#
# 思路：
#   1. 定义一个辅助函数 check_command(cmd, description)
#      - 使用 command -v 检测命令是否存在
#      - 不存在则打印错误信息并返回非零
# 
#   2. 检查 binutils 工具
#      - as (GNU Assembler, AT&T 语法)
#      - ld (GNU Linker)
#      - objcopy (用于二进制操作)
#
#   3. 检查 QEMU
#      - qemu-system-x86_64 (用于运行内核)
#
#   4. 检查 CMake
#      - cmake 版本 >= 4.1
#
#   5. 汇总结果
#      - 全部存在则返回 0，打印 "[OK] All tools found"
#      - 有缺失则返回 1，打印缺失列表
#      - 提供安装命令提示（sudo apt install ...）

# ============================================================
# 辅助函数定义
# ============================================================

# 检查命令是否存在，不存在则立即退出
# 参数:
#   $1 - 命令名称
#   $2 - 安装提示信息（可选）
check_command() {
    local cmd="$1"
    local install_hint="$2"

    if command -v "$cmd" &> /dev/null; then
        log_info "[OK] $cmd found"
        return 0
    else
        log_error "[MISSING] $cmd not found"
        if [[ -n "$install_hint" ]]; then
            log_error "  Install: $install_hint"
        fi
        exit 1
    fi
}

# 检查 cmake 版本是否满足要求
check_cmake_version() {
    local min_version="4.1"
    local current_version

    if ! command -v cmake &> /dev/null; then
        log_error "[MISSING] cmake not found"
        log_error "  Install: sudo apt install cmake"
        exit 1
    fi

    current_version=$(cmake --version | head -n1 | grep -oP '\d+\.\d+')
    if [[ $(echo "$current_version < $min_version" | bc -l) -eq 1 ]]; then
        log_error "[VERSION] cmake version $current_version < $min_version"
        log_error "  Upgrade: sudo apt install cmake"
        exit 1
    fi
    log_info "[OK] cmake $current_version (>= $min_version)"
}

# ============================================================
# 主检查逻辑
# ============================================================

log_info "Checking required tools..."

check_command "gcc" "sudo apt install gcc"
check_command "g++" "sudo apt install g++"
check_command "as" "sudo apt install binutils"
check_command "ld" "sudo apt install binutils"
check_command "objcopy" "sudo apt install binutils"
check_command "qemu-system-x86_64" "sudo apt install qemu-system-x86"
check_cmake_version

log_success "[OK] All required tools are installed!"
exit 0
