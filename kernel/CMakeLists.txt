#
# kernel/CMakeLists.txt
#
# @brief Cinux kernel module CMake configuration
#
# Manages both the mini kernel (bootstrap) and the big kernel.
# Big kernel sources are organised into subdirectory CMakeLists.txt
# files that add sources via target_sources().
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

# Object library: shared sources between production and test kernels.
# Subdirectory CMakeLists.txt files populate this via target_sources().
add_library(big_kernel_common OBJECT)

target_compile_options(big_kernel_common PUBLIC ${BIG_KERNEL_COMPILE_OPTIONS})

target_include_directories(big_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# Sub-modules (each adds its sources to big_kernel_common)
add_subdirectory(arch)
add_subdirectory(drivers)
add_subdirectory(fs)
add_subdirectory(lib)
add_subdirectory(mm)
add_subdirectory(proc)
add_subdirectory(syscall)
add_subdirectory(ipc)

# GUI mode: add compile definition and GUI-specific sources
if(CINUX_GUI)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_GUI)
    add_subdirectory(gui)
endif()

# ============================================================
# Initrd (embedded ustar archive)
# ============================================================

# Convert the initrd tar file into a linkable ELF object.
# This produces symbols _binary_initrd_start / _binary_initrd_end / _binary_initrd_size.
set(INITRD_TAR ${CMAKE_CURRENT_SOURCE_DIR}/data/initrd.tar)
set(INITRD_OBJ  ${CMAKE_BINARY_DIR}/kernel/initrd.o)

# Convert initrd.tar into a linkable ELF object with stable symbol names
# (_binary_initrd_start / _binary_initrd_end / _binary_initrd_size).
# Uses a helper script because objcopy derives symbol names from the
# absolute input path, making them non-portable.
set(EMBED_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/embed_binary.sh)

add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND bash ${EMBED_SCRIPT}
        ${INITRD_TAR} ${INITRD_OBJ} .initrd _binary_initrd
    DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}
    COMMENT "Converting initrd.tar -> initrd.o (embedded ramdisk)"
)

add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})

# Production big kernel executable
add_executable(big_kernel
    main.cpp
)

target_sources(big_kernel PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel PRIVATE big_kernel_common)
add_dependencies(big_kernel user_binary_obj initrd_obj)
target_sources(big_kernel PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)

set_target_properties(big_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/big
    OUTPUT_NAME "big_kernel"
)

target_link_options(big_kernel PRIVATE ${BIG_KERNEL_LINK_OPTIONS})

# ============================================================
# Big Kernel Test Binary (QEMU in-kernel tests)
# ============================================================
# Separate executable that replaces kernel_main with a test harness.
# Gets loaded by the mini kernel just like the production big kernel.

add_executable(big_kernel_test
    test/main_test.cpp
    test/test_gdt_idt.cpp
    test/test_pic_pit.cpp
    test/test_video.cpp
    test/test_keyboard.cpp
    test/test_pmm.cpp
    test/test_vmm.cpp
    test/test_heap.cpp
    test/test_address_space.cpp
    test/test_scheduler.cpp
    test/test_sync.cpp
    test/test_sync_concurrent.cpp
    test/test_usermode.cpp
    test/test_syscall.cpp
    test/test_shell.cpp
    test/test_ahci.cpp
    test/test_ramdisk.cpp
    test/test_vfs_syscall.cpp
    test/test_ext2.cpp
    test/test_ahci_write.cpp
    test/test_ext2_allocator.cpp
    test/test_ext2_ops.cpp
    test/test_ext2_inode_ops.cpp
    test/test_syscall_ext2.cpp
    test/test_shell_write.cpp
    test/test_cwd_stat.cpp
    test/test_canvas.cpp
    test/test_mouse_event.cpp
    test/test_window.cpp
    test/test_window_manager.cpp
    test/test_gui_integration.cpp
    test/test_bitmap_icon.cpp
    test/test_desktop.cpp
    test/test_terminal.cpp
    test/test_pipe.cpp
    test/test_sys_pipe.cpp
    test/test_terminal_shell.cpp
    test/test_fork_exec.cpp
    test/test_multi_terminal.cpp
    test/test_kprintf_format.cpp
)

target_sources(big_kernel_test PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel_test PRIVATE big_kernel_common)
add_dependencies(big_kernel_test user_binary_obj initrd_obj)
target_sources(big_kernel_test PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)

set_target_properties(big_kernel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/big
    OUTPUT_NAME "big_kernel_test"
)

target_link_options(big_kernel_test PRIVATE ${BIG_KERNEL_LINK_OPTIONS})

# Build test disk image: copy the test ELF to sectors where the big kernel lives
set(BIG_KERNEL_TEST_BIN "${CMAKE_BINARY_DIR}/kernel/big/big_kernel_test_crc.bin")

add_custom_command(TARGET big_kernel_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:big_kernel_test> ${BIG_KERNEL_TEST_BIN}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/append_crc32.py ${BIG_KERNEL_TEST_BIN}
    COMMENT "Creating big kernel test binary with trailing CRC32"
)
