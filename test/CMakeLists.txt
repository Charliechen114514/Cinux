# =============================================================================
# test/CMakeLists.txt
# @brief Cinux host-side unit test configuration
#
# Defines host-side tests that run on Linux with -DCINUX_HOST_TEST.
# Tests are registered via add_cinux_test() (simple) or
# add_cinux_integration_test() (links kernel sources).
# =============================================================================

set(CINUX_HOST_TEST ON)

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)

enable_testing()

# ============================================================
# Helper functions
# ============================================================

# Simple test: compiles unit/test_<name>.cpp with no extra sources.
function(add_cinux_test name)
    add_executable(test_${name} unit/test_${name}.cpp ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
endfunction()

# Integration test: arbitrary sources listed after the name.
function(add_cinux_integration_test name)
    add_executable(test_${name} ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/test/framework
        ${CMAKE_SOURCE_DIR}/kernel
        ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
endfunction()

# ============================================================
# Simple tests (pure arithmetic, no kernel code linked)
# ============================================================

add_cinux_test(smoke)
add_cinux_test(gdt_idt)
add_cinux_test(ata)
add_cinux_test(elf_loader)
add_cinux_test(big_kernel_loader)
add_cinux_test(pic)
add_cinux_test(pit)
add_cinux_test(font)
add_cinux_test(console)
add_cinux_test(framebuffer)
add_cinux_test(keyboard)
add_cinux_test(heap)
add_cinux_test(address_space)
add_cinux_test(scheduler)
add_cinux_test(sync)
add_cinux_test(usermode)
add_cinux_test(syscall)
add_cinux_test(shell)
add_cinux_test(ahci)
add_cinux_test(ext2)
add_cinux_test(ahci_write)
add_cinux_test(ext2_allocator)
add_cinux_test(ext2_ops)
add_cinux_test(syscall_ext2)
add_cinux_test(shell_write)
add_cinux_test(cwd_stat)
add_cinux_test(canvas)
add_cinux_test(bitmap_icon)
add_cinux_test(mouse)
add_cinux_test(event_queue)
add_cinux_test(window)
add_cinux_test(window_manager)
add_cinux_test(desktop)
add_cinux_test(terminal)
add_cinux_test(multi_terminal)

# ============================================================
# Tests with threading support
# ============================================================

add_cinux_test(pmm)
target_link_libraries(test_pmm PRIVATE -pthread)

add_cinux_test(sync_concurrent)
target_link_libraries(test_sync_concurrent PRIVATE -pthread)

# ============================================================
# Tests with C++23 requirement
# ============================================================

add_cinux_test(kprintf)
target_compile_features(test_kprintf PRIVATE cxx_std_23)

# ============================================================
# Tests with extra kernel sources
# ============================================================

add_cinux_integration_test(kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)

add_cinux_integration_test(vmm
    unit/test_vmm.cpp
)

add_cinux_integration_test(ramdisk
    unit/test_ramdisk.cpp
)

add_cinux_integration_test(fd_table
    unit/test_fd_table.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

add_cinux_integration_test(vfs_mount
    unit/test_vfs_mount.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

add_cinux_integration_test(ext2_inode_ops
    unit/test_ext2_inode_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

add_cinux_integration_test(pipe
    unit/test_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

add_cinux_integration_test(sys_pipe
    unit/test_sys_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

add_cinux_integration_test(shell_redirect
    unit/test_shell_redirect.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

add_cinux_integration_test(fork_exec
    unit/test_fork_exec.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/pid.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/elf_types.cpp
)

# ============================================================
# Custom test targets
# ============================================================

set(ALL_HOST_TESTS
    test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader
    test_big_kernel_loader test_kprintf test_pic test_pit test_font
    test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap
    test_address_space test_scheduler test_sync test_sync_concurrent
    test_usermode test_syscall test_shell test_ahci test_ramdisk test_fd_table
    test_vfs_mount test_ext2 test_ahci_write test_ext2_allocator test_ext2_ops
    test_ext2_inode_ops test_syscall_ext2 test_shell_write test_cwd_stat
    test_canvas test_bitmap_icon test_mouse test_event_queue test_window
    test_window_manager test_desktop test_terminal test_pipe test_sys_pipe
    test_shell_redirect test_fork_exec test_multi_terminal
)

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)

configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)

add_custom_target(test_all
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS ${ALL_HOST_TESTS} test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)

add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    COMMENT "Running host tests in verbose mode..."
)

add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
