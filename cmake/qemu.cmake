find_program(QEMU_EXECUTABLE qemu-system-x86_64)

if(NOT QEMU_EXECUTABLE)
    set(QEMU_EXECUTABLE "qemu-system-x86_64")
    message(WARNING "qemu-system-x86_64 not found in PATH, using default name")
endif()

set(QEMU_COMMON_FLAGS
    -m 8G
    -serial stdio
    -no-reboot
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
    -accel kvm -cpu max
    -usb -device usb-tablet
)

set(QEMU_DEVELOP_FLAG     
    -no-shutdown)

# AHCI test disk (1 MB, with MBR boot signature at offset 510-511)
set(AHCI_TEST_IMAGE "${CMAKE_BINARY_DIR}/ahci_test.img")
add_custom_command(
    OUTPUT ${AHCI_TEST_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/create_ahci_test_disk.sh ${AHCI_TEST_IMAGE}
    COMMENT "Creating AHCI test disk image"
    VERBATIM
)

# ext2 filesystem disk image (4 MB, mounted at AHCI port 1)
set(EXT2_IMAGE "${CMAKE_BINARY_DIR}/ext2.img")
add_custom_command(
    OUTPUT ${EXT2_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${EXT2_IMAGE}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh
    COMMENT "Creating ext2 filesystem image"
    VERBATIM
)

# QEMU 额外测试标志（添加到 COMMON_FLAGS 之上）
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
    -device ahci,id=ahci
    -drive file=${AHCI_TEST_IMAGE},format=raw,if=none,id=ahci-disk
    -device ide-hd,drive=ahci-disk,bus=ahci.0
    -drive file=${EXT2_IMAGE},format=raw,if=none,id=ext2-disk
    -device ide-hd,drive=ext2-disk,bus=ahci.1
)

# ============================================================
# isa-debug-exit Exit Code Mapping
# ============================================================
# QEMU's isa-debug-exit device encodes: exit_code = (value << 1) | 1
#   Kernel writes 0 → QEMU exits 1 → test SUCCESS
#   Kernel writes 1 → QEMU exits 3 → test FAILURE
# The run-kernel-test and run-stress-test targets use a bash wrapper
# to map QEMU exit code 1 → make success, 3 → make failure.

# 将 CMake list 转换为空格分隔的字符串（用于脚本生成）
string(REPLACE ";" " " QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS}")
string(REPLACE ";" " " QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS}")
set(QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS_STR}" CACHE INTERNAL "")
set(QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS_STR}" CACHE INTERNAL "")

# Set the debug console as 0xe9
# -s: GDB stub on :1234
# -S: Stop at startup (for debugging)
set(QEMU_DEBUG_FLAGS
    -s
    -S
)

if(NOT CINUX_IMAGE_PATH)
    message(STATUS "Image Path not specified yet, using default")
    set(CINUX_IMAGE_PATH "${CMAKE_BINARY_DIR}/cinux.img" CACHE PATH "Cinux disk image path")
endif()

# Let We make boots before sessions
set(MBR_BIN    "${CMAKE_BINARY_DIR}/boot/mbr.bin")
set(STAGE2_BIN "${CMAKE_BINARY_DIR}/boot/stage2.bin")
set(MINI_BIN   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel.bin")
set(BIG_KERNEL_BIN "${CMAKE_BINARY_DIR}/kernel/big/big_kernel")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
        ${BIG_KERNEL_BIN}
    DEPENDS mbr stage2 mini_kernel big_kernel
    COMMENT "Building disk image: ${CINUX_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(image ALL
    DEPENDS ${CINUX_IMAGE_PATH}
)

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
        -device ahci,id=ahci
        -drive file=${AHCI_TEST_IMAGE},format=raw,if=none,id=ahci-disk
        -device ide-hd,drive=ahci-disk,bus=ahci.0
        -drive file=${EXT2_IMAGE},format=raw,if=none,id=ext2-disk
        -device ide-hd,drive=ext2-disk,bus=ahci.1
    DEPENDS image ${AHCI_TEST_IMAGE} ${EXT2_IMAGE}
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)

add_custom_target(run-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU in debug mode (GDB on :1234)"
    VERBATIM
)


add_custom_target(run-gdb
    COMMAND gdb -ex "target remote :1234" build/kernel.elf
    DEPENDS run-debug
    COMMENT "Using gdb to connects for debugings"
    VERBATIM)

# ==============================================================
# Test Kernel Targets
# ==============================================================

set(MINI_TEST_BIN "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel_test.bin")
set(BIG_KERNEL_TEST_ELF "${CMAKE_BINARY_DIR}/kernel/big/big_kernel_test_crc.bin")

# 测试内核磁盘镜像
set(CINUX_TEST_IMAGE_PATH "${CMAKE_BINARY_DIR}/cinux_test.img")

add_custom_command(
    OUTPUT ${CINUX_TEST_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_TEST_BIN}
        ${CINUX_TEST_IMAGE_PATH}
        ${BIG_KERNEL_TEST_ELF}
    DEPENDS mbr stage2 mini_kernel_test big_kernel_test
    COMMENT "Building test disk image: ${CINUX_TEST_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(test-image
    DEPENDS ${CINUX_TEST_IMAGE_PATH}
)

# ==============================================================
# Stress Test Targets (1GB kernel load)
# ==============================================================

set(STRESS_KERNEL_ELF "${CMAKE_BINARY_DIR}/stress_kernel.elf")

add_custom_command(
    OUTPUT ${STRESS_KERNEL_ELF}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/generate_large_elf.py
        --size 1073741824
        --output ${STRESS_KERNEL_ELF}
    COMMENT "Generating 1GB stress test ELF"
    VERBATIM
)

add_custom_target(stress-kernel-elf
    DEPENDS ${STRESS_KERNEL_ELF}
)

# Stress test disk image: mini test kernel + 1GB synthetic ELF
set(STRESS_TEST_IMAGE "${CMAKE_BINARY_DIR}/cinux_stress_test.img")

add_custom_command(
    OUTPUT ${STRESS_TEST_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_TEST_BIN}
        ${STRESS_TEST_IMAGE}
        ${STRESS_KERNEL_ELF}
    DEPENDS mbr stage2 mini_kernel_test stress-kernel-elf
    COMMENT "Building stress test disk image (1GB kernel)"
    VERBATIM
)

add_custom_target(stress-test-image
    DEPENDS ${STRESS_TEST_IMAGE}
)

# ==============================================================
# Big Kernel Test Target (production mini kernel + test big kernel)
# ==============================================================
# Uses the production mini kernel (which loads and jumps to big kernel)
# with the big_kernel_test binary (which has a test main instead of production main).

set(BIG_KERNEL_QEMU_TEST_ELF "${CMAKE_BINARY_DIR}/kernel/big/big_kernel_test")
set(BIG_KERNEL_QEMU_TEST_IMAGE "${CMAKE_BINARY_DIR}/cinux_big_test.img")

add_custom_command(
    OUTPUT ${BIG_KERNEL_QEMU_TEST_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN} ${STAGE2_BIN} ${MINI_BIN}
        ${BIG_KERNEL_QEMU_TEST_IMAGE}
        ${BIG_KERNEL_QEMU_TEST_ELF}
    DEPENDS mbr stage2 mini_kernel big_kernel_test
    COMMENT "Building big kernel test disk image"
    VERBATIM
)

add_custom_target(big-kernel-test-image
    DEPENDS ${BIG_KERNEL_QEMU_TEST_IMAGE}
)

add_custom_target(run-big-kernel-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${BIG_KERNEL_QEMU_TEST_IMAGE},format=raw,index=0,media=disk
    DEPENDS big-kernel-test-image
    USES_TERMINAL
    COMMENT "Running big kernel GDT/IDT tests in QEMU"
    VERBATIM
)

add_custom_target(run-stress-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${STRESS_TEST_IMAGE},format=raw,index=0,media=disk,cache=unsafe
    DEPENDS stress-test-image
    USES_TERMINAL
    COMMENT "Running 1GB kernel stress test"
    VERBATIM
)

# 运行测试内核（自动退出模式）
# 每次 run-kernel-test 前强制重建 ext2.img，确保磁盘状态干净
add_custom_target(regenerate-ext2-image
    COMMAND ${CMAKE_COMMAND} -E remove -f ${EXT2_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${EXT2_IMAGE}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh
    COMMENT "Regenerating ext2 disk image for clean test state"
    VERBATIM
)

add_custom_target(run-kernel-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image ${AHCI_TEST_IMAGE} regenerate-ext2-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (auto-exit)"
    VERBATIM
)

# 测试内核调试模式
add_custom_target(run-kernel-test-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    COMMENT "Starting QEMU with TEST kernel in debug mode (GDB on :1234)"
    VERBATIM
)

# 交互式运行测试内核（需要 Ctrl+C 退出）
add_custom_target(run-kernel-test-interactive
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (interactive, Ctrl+C to exit)"
    VERBATIM
)

message(STATUS "QEMU targets:")
message(STATUS "  make run        : Start QEMU normally")
message(STATUS "  make run-debug  : Start QEMU with GDB server on :1234")
message(STATUS "  make image      : Build disk image only")
message(STATUS "  make run-gdb    : Connects the qemu automatically")
message(STATUS "")
message(STATUS "Test Kernel targets:")
message(STATUS "  make run-kernel-test            : Run mini kernel tests (auto-exit)")
message(STATUS "  make run-big-kernel-test        : Run big kernel GDT/IDT tests (auto-exit)")
message(STATUS "  make run-kernel-test-interactive : Run test kernel (needs Ctrl+C)")
message(STATUS "  make run-kernel-test-debug      : Run test kernel with GDB")
message(STATUS "  make test-image                  : Build test disk image only")
message(STATUS "")
message(STATUS "Stress Test targets:")
message(STATUS "  make stress-kernel-elf  : Generate 1GB synthetic ELF")
message(STATUS "  make stress-test-image  : Build stress test disk image")
message(STATUS "  make run-stress-test    : Run 1GB kernel stress test")
message(STATUS "")
message(STATUS "Unified Testing:")
message(STATUS "  make test                  : Run ALL tests (host + kernel, auto-exit)")
message(STATUS "  make test_host             : Run host unit tests only")
message(STATUS "  make test_verbose          : Run host tests in verbose mode")