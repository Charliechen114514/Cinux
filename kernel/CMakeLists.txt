#
# kernel/CMakeLists.txt
#
# @brief Cinux kernel module CMake configuration
#
# Manages both the mini kernel (bootstrap) and the big kernel.
#

# ============================================================
# Mini Kernel (Bootstrap Kernel)
# ============================================================
add_subdirectory(mini)

# ============================================================
# Big Kernel - Full-featured kernel loaded by mini kernel
# ============================================================

# Compile options for the big kernel
set(BIG_KERNEL_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -fno-stack-protector
    -mcmodel=kernel
    -mno-red-zone
    -Wall
    -Wextra
)

set(BIG_KERNEL_LINK_OPTIONS
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -static
    -no-pie
)

# Object library: shared sources between production and test kernels
add_library(big_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    drivers/serial.cpp
    lib/kprintf.cpp
)

target_compile_options(big_kernel_common PUBLIC ${BIG_KERNEL_COMPILE_OPTIONS})

target_include_directories(big_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# Production big kernel executable
add_executable(big_kernel
    main.cpp
)

target_sources(big_kernel PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel PRIVATE big_kernel_common)

set_target_properties(big_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/big
    OUTPUT_NAME "big_kernel"
)

target_link_options(big_kernel PRIVATE ${BIG_KERNEL_LINK_OPTIONS})

# ============================================================
# Big Kernel Test Binary (with trailing CRC32)
# ============================================================
# Append a CRC32 checksum to the big kernel ELF for integrity
# verification during kernel tests.
#
# Layout: [ELF data] [4-byte CRC32 LE] [padding to 512B alignment]

set(BIG_KERNEL_TEST_BIN "${CMAKE_BINARY_DIR}/kernel/big/big_kernel_test.bin")

add_custom_command(TARGET big_kernel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:big_kernel> ${BIG_KERNEL_TEST_BIN}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/append_crc32.py ${BIG_KERNEL_TEST_BIN}
    COMMENT "Creating big kernel test binary with trailing CRC32"
)
