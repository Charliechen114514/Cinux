find_program(QEMU_EXECUTABLE qemu-system-x86_64)

if(NOT QEMU_EXECUTABLE)
    set(QEMU_EXECUTABLE "qemu-system-x86_64")
    message(WARNING "qemu-system-x86_64 not found in PATH, using default name")
endif()

set(QEMU_COMMON_FLAGS
    -m 512M
    -serial stdio
    -no-reboot
    -no-shutdown
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)

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
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
    COMMENT "Building disk image: ${CINUX_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(image ALL
    DEPENDS ${CINUX_IMAGE_PATH}
)

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)

add_custom_target(run-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEBUG_FLAGS}
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

message(STATUS "QEMU targets:")
message(STATUS "  make run        : Start QEMU normally")
message(STATUS "  make run-debug  : Start QEMU with GDB server on :1234")
message(STATUS "  make image      : Build disk image only")
message(STATUS "  make run-gdb    : Connects the qemu automatically")