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
    arch/x86_64/gdt.cpp
    arch/x86_64/idt.cpp
    arch/x86_64/interrupts.S
    arch/x86_64/exception_handlers.cpp
    arch/x86_64/irq_handlers.cpp
    arch/x86_64/pic.cpp
    arch/x86_64/paging.cpp
    drivers/pit/pit.cpp
    drivers/serial/serial.cpp
    drivers/keyboard/keyboard.cpp
    drivers/video/framebuffer.cpp
    drivers/video/font.cpp
    drivers/video/font_data.S
    drivers/video/console.cpp
    drivers/pci/pci.cpp
    drivers/ahci/ahci.cpp
    fs/inode.cpp
    fs/ramdisk.cpp
    fs/ext2.cpp
    fs/file.cpp
    fs/vfs_mount.cpp
    fs/path.cpp
    lib/kprintf.cpp
    lib/string.cpp
    mm/pmm.cpp
    mm/vmm.cpp
    mm/heap.cpp
    mm/address_space.cpp
    proc/process.cpp
    proc/scheduler.cpp
    proc/sync.cpp
    proc/init.cpp
    arch/x86_64/context_switch.S
    arch/x86_64/usermode.S
    arch/x86_64/usermode.cpp
    arch/x86_64/syscall.S
    arch/x86_64/syscall.cpp
    syscall/sys_read.cpp
    syscall/sys_write.cpp
    syscall/sys_open.cpp
    syscall/sys_close.cpp
    syscall/sys_exit.cpp
    syscall/sys_yield.cpp
    syscall/sys_getdents.cpp
    syscall/sys_creat.cpp
    syscall/sys_mkdir.cpp
    syscall/sys_unlink.cpp
    syscall/sys_rmdir.cpp
    syscall/path_util.cpp
    syscall/sys_chdir.cpp
    syscall/sys_getcwd.cpp
    syscall/sys_stat.cpp
)

target_compile_options(big_kernel_common PUBLIC ${BIG_KERNEL_COMPILE_OPTIONS})

target_include_directories(big_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# GUI mode: add compile definition and extra sources
if(CINUX_GUI)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        drivers/canvas.cpp
        drivers/mouse.cpp
    )
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
