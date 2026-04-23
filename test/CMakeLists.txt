# =============================================================================
# test/CMakeLists.txt
# @brief Cinux 测试配置
#
# 此文件配置 Cinux 的 host 端单元测试。
# 测试在宿主机（Linux）上运行，使用 -DCINUX_HOST_TEST 编译标志。
#
# 测试文件位置：
#   - test/unit/test_*.cpp : 单元测试
#   - test/integration/*.cpp : 集成测试（后续）
#
# 测试框架：
#   - test/framework/test_framework.h : 自研轻量测试框架
# =============================================================================

# ============================================================
# 测试编译选项
# ============================================================

# 设置测试专用编译定义
# 添加 CINUX_HOST_TEST 宏，告诉代码在 host 端运行
set(CINUX_HOST_TEST ON)

# 设置测试包含目录
# 添加测试框架和内核头文件目录
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

# ============================================================
# CTest 配置
# ============================================================

# 启用 CMake 的 CTest 功能
enable_testing()

# ============================================================
# 测试可执行文件：test_smoke
# ============================================================

# 创建冒烟测试可执行文件
# 这是最基础的测试，验证测试框架本身工作正常
add_executable(test_smoke
    unit/test_smoke.cpp
)

# 添加编译定义：CINUX_HOST_TEST
target_compile_definitions(test_smoke PRIVATE CINUX_HOST_TEST)

# 设置包含目录
target_include_directories(test_smoke PRIVATE ${TEST_INCLUDE_DIRS})

# 注册测试到 CTest
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES LABELS "smoke")

# ============================================================
# 测试可执行文件：test_kprintf_format
# ============================================================
add_executable(test_kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)

target_compile_definitions(test_kprintf_format PRIVATE CINUX_HOST_TEST)
target_include_directories(test_kprintf_format PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME kprintf_format COMMAND test_kprintf_format)
set_tests_properties(kprintf_format PROPERTIES LABELS "kprintf_format")

# ============================================================
# 测试可执行文件：test_gdt_idt
# ============================================================
# GDT/IDT 数据结构与编码逻辑的 host 端单元测试
# 测试 GDT 描述符编码、IDT 条目编码、结构体布局、段选择子常量
add_executable(test_gdt_idt
    unit/test_gdt_idt.cpp
)

target_compile_definitions(test_gdt_idt PRIVATE CINUX_HOST_TEST)
target_include_directories(test_gdt_idt PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME gdt_idt COMMAND test_gdt_idt)
set_tests_properties(gdt_idt PROPERTIES LABELS "gdt_idt")

# ============================================================
# 测试可执行文件：test_ata
# ============================================================
# ATA PIO driver constants and register encoding logic tests
# Pure arithmetic tests (no kernel code linked)
add_executable(test_ata
    unit/test_ata.cpp
)

target_compile_definitions(test_ata PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ata PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ata COMMAND test_ata)
set_tests_properties(ata PROPERTIES LABELS "ata")

# ============================================================
# 测试可执行文件：test_elf_loader
# ============================================================
# ELF64 header validation, segment parsing, and loading logic tests
# Includes elf_loader.cpp directly for host-side testing of pure logic
add_executable(test_elf_loader
    unit/test_elf_loader.cpp
)

target_compile_definitions(test_elf_loader PRIVATE CINUX_HOST_TEST)
target_include_directories(test_elf_loader PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME elf_loader COMMAND test_elf_loader)
set_tests_properties(elf_loader PROPERTIES LABELS "elf_loader")

# ============================================================
# 测试可执行文件：test_big_kernel_loader
# ============================================================
# Big kernel loader constants, memory layout, and pipeline logic tests
# Pure constant and arithmetic verification (no kernel code linked)
add_executable(test_big_kernel_loader
    unit/test_big_kernel_loader.cpp
)

target_compile_definitions(test_big_kernel_loader PRIVATE CINUX_HOST_TEST)
target_include_directories(test_big_kernel_loader PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME big_kernel_loader COMMAND test_big_kernel_loader)
set_tests_properties(big_kernel_loader PROPERTIES LABELS "big_kernel_loader")

# ============================================================
# 测试可执行文件：test_pic
# ============================================================
# 8259A PIC driver constants, ICW encoding, EOI logic, and IMR
# mask/unmask bit manipulation tests.  Pure arithmetic -- no kernel
# code linked.
add_executable(test_pic
    unit/test_pic.cpp
)

target_compile_definitions(test_pic PRIVATE CINUX_HOST_TEST)
target_include_directories(test_pic PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME pic COMMAND test_pic)
set_tests_properties(pic PROPERTIES LABELS "pic")

# ============================================================
# 测试可执行文件：test_pit
# ============================================================
# Intel 8254 PIT driver constants, divisor calculation, command byte
# composition, and uptime tracking logic tests.  Pure arithmetic --
# no kernel code linked.
add_executable(test_pit
    unit/test_pit.cpp
)

target_compile_definitions(test_pit PRIVATE CINUX_HOST_TEST)
target_include_directories(test_pit PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME pit COMMAND test_pit)
set_tests_properties(pit PROPERTIES LABELS "pit")

# ============================================================
# 测试可执行文件：test_kprintf (big kernel kprintf formatting engine)
# ============================================================
# Tests the real vkprintf_impl template from the big kernel.
# The engine is header-only (kernel/lib/private/vkprintf_impl.hpp)
# so no .cpp sources are linked -- just the test file.
add_executable(test_kprintf
    unit/test_kprintf.cpp
)

target_compile_definitions(test_kprintf PRIVATE CINUX_HOST_TEST)
target_include_directories(test_kprintf PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(test_kprintf PRIVATE cxx_std_23)
add_test(NAME kprintf COMMAND test_kprintf)
set_tests_properties(kprintf PROPERTIES LABELS "kprintf")

# ============================================================
# 测试可执行文件：test_font
# ============================================================
# PSF2 header parsing, glyph offset arithmetic, and bitmap
# bit extraction logic.  Pure arithmetic -- no kernel code linked.
add_executable(test_font
    unit/test_font.cpp
)

target_compile_definitions(test_font PRIVATE CINUX_HOST_TEST)
target_include_directories(test_font PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME font COMMAND test_font)
set_tests_properties(font PROPERTIES LABELS "font")

# ============================================================
# 测试可执行文件：test_console
# ============================================================
# Console cursor logic: column/row tracking, wrapping, scrolling
# triggers, and control character handling.  Pure arithmetic.
add_executable(test_console
    unit/test_console.cpp
)

target_compile_definitions(test_console PRIVATE CINUX_HOST_TEST)
target_include_directories(test_console PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME console COMMAND test_console)
set_tests_properties(console PROPERTIES LABELS "console")

# ============================================================
# 测试可执行文件：test_framebuffer
# ============================================================
# Framebuffer pitch-based indexing, bounds checking, fill_rect
# region calculation, and scroll_up byte offset arithmetic.
add_executable(test_framebuffer
    unit/test_framebuffer.cpp
)

target_compile_definitions(test_framebuffer PRIVATE CINUX_HOST_TEST)
target_include_directories(test_framebuffer PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME framebuffer COMMAND test_framebuffer)
set_tests_properties(framebuffer PROPERTIES LABELS "framebuffer")

# ============================================================
# 测试可执行文件：test_keyboard
# ============================================================
# PS/2 keyboard driver scan code conversion, modifier state tracking,
# and ring buffer logic tests.  Pure arithmetic -- no kernel code linked.
add_executable(test_keyboard
    unit/test_keyboard.cpp
)

target_compile_definitions(test_keyboard PRIVATE CINUX_HOST_TEST)
target_include_directories(test_keyboard PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME keyboard COMMAND test_keyboard)
set_tests_properties(keyboard PROPERTIES LABELS "keyboard")

# ============================================================
# 测试可执行文件：test_pmm
# ============================================================
# Physical Memory Manager: parse_memory_map logic and bitmap
# allocator algorithm tests.  Pure arithmetic -- no kernel code linked.
add_executable(test_pmm
    unit/test_pmm.cpp
)

target_compile_definitions(test_pmm PRIVATE CINUX_HOST_TEST)
target_include_directories(test_pmm PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(test_pmm PRIVATE -pthread)
add_test(NAME pmm COMMAND test_pmm)
set_tests_properties(pmm PROPERTIES LABELS "pmm")

# ============================================================
# 测试可执行文件：test_vmm
# ============================================================
# Virtual Memory Manager: map/unmap/translate logic with mock PMM.
# Pure arithmetic -- no kernel code linked.
add_executable(test_vmm
    unit/test_vmm.cpp
)

target_compile_definitions(test_vmm PRIVATE CINUX_HOST_TEST)
target_include_directories(test_vmm PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME vmm COMMAND test_vmm)
set_tests_properties(vmm PROPERTIES LABELS "vmm")

# ============================================================
# 测试可执行文件：test_heap
# ============================================================
# Kernel heap allocator: first-fit, split, coalesce, alignment,
# double-free detection, 1000-cycle random alloc/free leak check.
# Pure reimplementation -- no kernel code linked.
add_executable(test_heap
    unit/test_heap.cpp
)

target_compile_definitions(test_heap PRIVATE CINUX_HOST_TEST)
target_include_directories(test_heap PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME heap COMMAND test_heap)
set_tests_properties(heap PROPERTIES LABELS "heap")

# ============================================================
# 测试可执行文件：test_address_space
# ============================================================
# Per-process virtual address space: construction, destruction,
# move semantics, map/unmap/translate, cross-space isolation.
# Pure reimplementation -- no kernel code linked.
add_executable(test_address_space
    unit/test_address_space.cpp
)

target_compile_definitions(test_address_space PRIVATE CINUX_HOST_TEST)
target_include_directories(test_address_space PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME address_space COMMAND test_address_space)
set_tests_properties(address_space PROPERTIES LABELS "address_space")

# ============================================================
# 测试可执行文件：test_scheduler
# ============================================================
# Process/scheduler: RoundRobin enqueue/dequeue/pick_next, Scheduler
# add_task/yield, CpuContext layout, TaskState enum.  Pure
# reimplementation -- no kernel code linked.
add_executable(test_scheduler
    unit/test_scheduler.cpp
)

target_compile_definitions(test_scheduler PRIVATE CINUX_HOST_TEST)
target_include_directories(test_scheduler PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME scheduler COMMAND test_scheduler)
set_tests_properties(scheduler PROPERTIES LABELS "scheduler")

# ============================================================
# 测试可执行文件：test_sync
# ============================================================
# Synchronization primitives: Spinlock, Mutex, Semaphore,
# wait queue enqueue/dequeue, FIFO ordering, RAII guard.
# Pure reimplementation -- no kernel code linked.
add_executable(test_sync
    unit/test_sync.cpp
)

target_compile_definitions(test_sync PRIVATE CINUX_HOST_TEST)
target_include_directories(test_sync PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME sync COMMAND test_sync)
set_tests_properties(sync PROPERTIES LABELS "sync")

# ============================================================
# 测试可执行文件：test_usermode
# ============================================================
# User-mode (Ring 3) transition: constants, STAR MSR computation,
# TSS layout, user page flags, bytecode encoding, address layout.
# Pure arithmetic -- no kernel code linked.
add_executable(test_usermode
    unit/test_usermode.cpp
)

target_compile_definitions(test_usermode PRIVATE CINUX_HOST_TEST)
target_include_directories(test_usermode PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME usermode COMMAND test_usermode)
set_tests_properties(usermode PROPERTIES LABELS "usermode")

# ============================================================
# 测试可执行文件：test_syscall
# ============================================================
# Syscall infrastructure: number constants, dispatch table logic,
# sys_write parameter validation, sys_exit state transition,
# STAR MSR computation.  Pure reimplementation -- no kernel code linked.
add_executable(test_syscall
    unit/test_syscall.cpp
)

target_compile_definitions(test_syscall PRIVATE CINUX_HOST_TEST)
target_include_directories(test_syscall PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME syscall COMMAND test_syscall)
set_tests_properties(syscall PROPERTIES LABELS "syscall")

# ============================================================
# 测试可执行文件：test_shell
# ============================================================
# Shell infrastructure: string utilities (strlen, strcmp, memset,
# memcpy, memcmp), tokenizer, command dispatch table, cmd_echo,
# cmd_help, cmd_clear, read_line with backspace handling.
# Pure reimplementation -- no kernel code linked.
add_executable(test_shell
    unit/test_shell.cpp
)

target_compile_definitions(test_shell PRIVATE CINUX_HOST_TEST)
target_include_directories(test_shell PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME shell COMMAND test_shell)
set_tests_properties(shell PROPERTIES LABELS "shell")

# ============================================================
# 测试可执行文件：test_ahci
# ============================================================
# AHCI SATA driver: register layout, FIS construction, PCI constants,
# PRDT setup logic, CFIS LBA/count encoding.  Pure arithmetic --
# no kernel code linked.
add_executable(test_ahci
    unit/test_ahci.cpp
)

target_compile_definitions(test_ahci PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ahci PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ahci COMMAND test_ahci)
set_tests_properties(ahci PROPERTIES LABELS "ahci")

# ============================================================
# 测试可执行文件：test_ramdisk
# ============================================================
# Ramdisk / ustar archive parser: header layout, octal conversion,
# mount traversal, data block computation, type filtering.
# Pure arithmetic -- no kernel code linked.
add_executable(test_ramdisk
    unit/test_ramdisk.cpp
)

target_compile_definitions(test_ramdisk PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ramdisk PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME ramdisk COMMAND test_ramdisk)
set_tests_properties(ramdisk PROPERTIES LABELS "ramdisk")

# ============================================================
# 测试可执行文件：test_fd_table
# ============================================================
# FDTable: per-process file descriptor table -- alloc/close/get
# Links directly with kernel/fs/file.cpp (no hardware dependencies).
add_executable(test_fd_table
    unit/test_fd_table.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

target_compile_definitions(test_fd_table PRIVATE CINUX_HOST_TEST)
target_include_directories(test_fd_table PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME fd_table COMMAND test_fd_table)
set_tests_properties(fd_table PROPERTIES LABELS "fd_table")

# ============================================================
# 测试可执行文件：test_vfs_mount
# ============================================================
# VFS mount table: init/add/remove/resolve, longest-prefix match,
# global FDTable accessor.
# Links with kernel/fs/vfs_mount.cpp and kernel/fs/file.cpp.
add_executable(test_vfs_mount
    unit/test_vfs_mount.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

target_compile_definitions(test_vfs_mount PRIVATE CINUX_HOST_TEST)
target_include_directories(test_vfs_mount PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME vfs_mount COMMAND test_vfs_mount)
set_tests_properties(vfs_mount PROPERTIES LABELS "vfs_mount")

# ============================================================
# 测试可执行文件：test_ext2
# ============================================================
# ext2 filesystem: packed struct sizes, field offsets, block_size
# computation, group/inode offset arithmetic, constants, enum values.
# Pure arithmetic -- no kernel code linked.
add_executable(test_ext2
    unit/test_ext2.cpp
)

target_compile_definitions(test_ext2 PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ext2 PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ext2 COMMAND test_ext2)
set_tests_properties(ext2 PROPERTIES LABELS "ext2")

# ============================================================
# 测试可执行文件：test_ahci_write
# ============================================================
# AHCI write path and ext2 write_block logic: CFIS write command encoding,
# command header write flag, block-to-LBA arithmetic, PRDT byte count,
# read-modify-write pattern verification.  Pure arithmetic -- no kernel
# code linked.
add_executable(test_ahci_write
    unit/test_ahci_write.cpp
)

target_compile_definitions(test_ahci_write PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ahci_write PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ahci_write COMMAND test_ahci_write)
set_tests_properties(ahci_write PROPERTIES LABELS "ahci_write")

# ============================================================
# 测试可执行文件：test_ext2_allocator
# ============================================================
# ext2 block/inode allocator: bitmap scanning, bit manipulation,
# cross-group allocation, free-then-realloc, counter consistency,
# boundary conditions (last group, full filesystem, zero input).
# Pure arithmetic -- no kernel code linked.
add_executable(test_ext2_allocator
    unit/test_ext2_allocator.cpp
)

target_compile_definitions(test_ext2_allocator PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ext2_allocator PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ext2_allocator COMMAND test_ext2_allocator)
set_tests_properties(ext2_allocator PROPERTIES LABELS "ext2_allocator")

# ============================================================
# 测试可执行文件：test_ext2_ops
# ============================================================
# ext2 write/create/mkdir/unlink operations: file write (single,
# cross-block, multi-block), create inode+dir entry, mkdir with
# dot/dotdot, unlink with block+inode free, add_dir_entry splitting,
# remove_dir_entry merge, full flow round-trip.
# Pure arithmetic -- no kernel code linked.
add_executable(test_ext2_ops
    unit/test_ext2_ops.cpp
)

target_compile_definitions(test_ext2_ops PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ext2_ops PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ext2_ops COMMAND test_ext2_ops)
set_tests_properties(ext2_ops PROPERTIES LABELS "ext2_ops")

# ============================================================
# 测试可执行文件：test_ext2_inode_ops
# ============================================================
# InodeOps virtual class hierarchy: default implementations,
# MockFileOps/MockDirOps override dispatch, base-class pointer
# virtual dispatch, Inode ops pointer assignment for file vs dir.
# Pure C++ virtual dispatch testing -- no kernel code linked.
add_executable(test_ext2_inode_ops
    unit/test_ext2_inode_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

target_compile_definitions(test_ext2_inode_ops PRIVATE CINUX_HOST_TEST)
target_include_directories(test_ext2_inode_ops PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME ext2_inode_ops COMMAND test_ext2_inode_ops)
set_tests_properties(ext2_inode_ops PROPERTIES LABELS "ext2_inode_ops")

# ============================================================
# 测试可执行文件：test_syscall_ext2
# ============================================================
# sys_creat/sys_mkdir/sys_unlink/sys_rmdir: split_pathname, path
# resolution, canonical address validation, VFS dispatch, ops call.
# Pure reimplementation -- no kernel code linked.
add_executable(test_syscall_ext2
    unit/test_syscall_ext2.cpp
)

target_compile_definitions(test_syscall_ext2 PRIVATE CINUX_HOST_TEST)
target_include_directories(test_syscall_ext2 PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME syscall_ext2 COMMAND test_syscall_ext2)
set_tests_properties(syscall_ext2 PROPERTIES LABELS "syscall_ext2")

# ============================================================
# 测试可执行文件：test_shell_write
# ============================================================
# Shell write commands: touch, mkdir, rm, rmdir, echo redirect.
# Command parsing, usage messages, error output verification.
# Pure reimplementation with mock syscalls -- no kernel code linked.
add_executable(test_shell_write
    unit/test_shell_write.cpp
)

target_compile_definitions(test_shell_write PRIVATE CINUX_HOST_TEST)
target_include_directories(test_shell_write PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME shell_write COMMAND test_shell_write)
set_tests_properties(shell_write PROPERTIES LABELS "shell_write")

# ============================================================
# 测试可执行文件：test_cwd_stat
# ============================================================
# CWD/stat: path_canonicalize, path_resolve, struct stat layout,
# shell cd/pwd/stat commands.  Pure reimplementation with mock
# syscalls -- no kernel code linked.
add_executable(test_cwd_stat
    unit/test_cwd_stat.cpp
)

target_compile_definitions(test_cwd_stat PRIVATE CINUX_HOST_TEST)
target_include_directories(test_cwd_stat PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME cwd_stat COMMAND test_cwd_stat)
set_tests_properties(cwd_stat PROPERTIES LABELS "cwd_stat")

# ============================================================
# 测试可执行文件：test_sync_concurrent
# ============================================================
# Concurrent stress tests for synchronization primitives using real
# OS threads (std::thread).  Verifies Spinlock correctness under
# genuine concurrent access, InterruptGuard nesting, IrqGuard.
add_executable(test_sync_concurrent
    unit/test_sync_concurrent.cpp
)

target_compile_definitions(test_sync_concurrent PRIVATE CINUX_HOST_TEST)
target_include_directories(test_sync_concurrent PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_sync_concurrent PRIVATE -pthread)
add_test(NAME sync_concurrent COMMAND test_sync_concurrent)
set_tests_properties(sync_concurrent PROPERTIES LABELS "sync_concurrent")

# ============================================================
# 测试可执行文件：test_canvas
# ============================================================
# Canvas double-buffered drawing: draw_pixel, draw_rect,
# draw_rect_outline, clear, flip, pitch handling.
# Pure reimplementation with mock Framebuffer -- no kernel code linked.
add_executable(test_canvas
    unit/test_canvas.cpp
)

target_compile_definitions(test_canvas PRIVATE CINUX_HOST_TEST)
target_include_directories(test_canvas PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME canvas COMMAND test_canvas)
set_tests_properties(canvas PROPERTIES LABELS "canvas")

# ============================================================
# 测试可执行文件：test_mouse
# ============================================================
# PS/2 mouse driver: init command sequence, 3-byte packet parsing,
# 9-bit signed dx/dy, button edge detection, cursor clamping.
# Pure reimplementation with mock I/O -- no kernel code linked.
add_executable(test_mouse
    unit/test_mouse.cpp
)

target_compile_definitions(test_mouse PRIVATE CINUX_HOST_TEST)
target_include_directories(test_mouse PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME mouse COMMAND test_mouse)
set_tests_properties(mouse PROPERTIES LABELS "mouse")

# ============================================================
# 测试可执行文件：test_event_queue
# ============================================================
# GUI EventQueue ring buffer: enqueue/dequeue, full/empty, wrap-around,
# clear, mixed event types, FIFO ordering.
# Pure reimplementation -- no kernel code linked.
add_executable(test_event_queue
    unit/test_event_queue.cpp
)

target_compile_definitions(test_event_queue PRIVATE CINUX_HOST_TEST)
target_include_directories(test_event_queue PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME event_queue COMMAND test_event_queue)
set_tests_properties(event_queue PROPERTIES LABELS "event_queue")

# ============================================================
# 测试可执行文件：test_window
# ============================================================
# Window class: construction, id auto-increment, title bar drawing,
# content drawing, close button hit testing, contains, set_position,
# resize, set_title, blit_to, canvas allocation.
# Pure reimplementation with mock Canvas/PSFFont -- no kernel code linked.
add_executable(test_window
    unit/test_window.cpp
)

target_compile_definitions(test_window PRIVATE CINUX_HOST_TEST)
target_include_directories(test_window PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME window COMMAND test_window)
set_tests_properties(window PROPERTIES LABELS "window")

# ============================================================
# 测试可执行文件：test_window_manager
# ============================================================
# WindowManager: init, create, destroy, raise, composite, handle_mouse,
# handle_key, Z-ordering, drag support.
# Pure reimplementation with mock Canvas/PSFFont -- no kernel code linked.
add_executable(test_window_manager
    unit/test_window_manager.cpp
)

target_compile_definitions(test_window_manager PRIVATE CINUX_HOST_TEST)
target_include_directories(test_window_manager PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME window_manager COMMAND test_window_manager)
set_tests_properties(window_manager PROPERTIES LABELS "window_manager")

# ============================================================
# 测试可执行文件：test_terminal
# ============================================================
# Terminal window: 80x25 char buffer, cursor, write, ANSI sequences,
# scroll, backspace, tab, clear, on_key.
# Pure reimplementation with mock Canvas/PSFFont -- no kernel code linked.
add_executable(test_terminal
    unit/test_terminal.cpp
)

target_compile_definitions(test_terminal PRIVATE CINUX_HOST_TEST)
target_include_directories(test_terminal PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME terminal COMMAND test_terminal)
set_tests_properties(terminal PROPERTIES LABELS "terminal")

# ============================================================
# 测试可执行文件：test_pipe
# ============================================================
# Pipe ring-buffer IPC: write/read, close semantics, EOF, state queries,
# wrap-around, PipeReadOps/PipeWriteOps direction restrictions.
# Links with kernel/ipc/pipe.cpp, kernel/ipc/pipe_ops.cpp,
# kernel/fs/inode.cpp, and unit/host_spinlock.cpp for Spinlock.
add_executable(test_pipe
    unit/test_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

target_compile_definitions(test_pipe PRIVATE CINUX_HOST_TEST)
target_include_directories(test_pipe PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME pipe COMMAND test_pipe)
set_tests_properties(pipe PROPERTIES LABELS "pipe")

# ============================================================
# 测试可执行文件：test_sys_pipe
# ============================================================
# sys_pipe integration: FDTable::set(), Pipe + InodeOps round-trip,
# close semantics through FDTable indirection.
# Links with kernel/fs/file.cpp, kernel/ipc/pipe.cpp, kernel/ipc/pipe_ops.cpp,
# kernel/fs/inode.cpp, and unit/host_spinlock.cpp.
add_executable(test_sys_pipe
    unit/test_sys_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

target_compile_definitions(test_sys_pipe PRIVATE CINUX_HOST_TEST)
target_include_directories(test_sys_pipe PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME sys_pipe COMMAND test_sys_pipe)
set_tests_properties(sys_pipe PROPERTIES LABELS "sys_pipe")

# ============================================================
# 测试可执行文件：test_shell_redirect
# ============================================================
# Shell stdin/stdout pipe redirection: FDTable pipe binding,
# InodeOps read/write through pipes, round-trip data flow,
# try_read/try_write non-blocking polling.
# Links with kernel/fs/file.cpp, kernel/fs/inode.cpp,
# kernel/fs/vfs_mount.cpp, kernel/ipc/pipe.cpp, kernel/ipc/pipe_ops.cpp,
# and unit/host_spinlock.cpp.
add_executable(test_shell_redirect
    unit/test_shell_redirect.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

target_compile_definitions(test_shell_redirect PRIVATE CINUX_HOST_TEST)
target_include_directories(test_shell_redirect PRIVATE
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
)
add_test(NAME shell_redirect COMMAND test_shell_redirect)
set_tests_properties(shell_redirect PROPERTIES LABELS "shell_redirect")

# ============================================================
# 测试辅助目标
# ============================================================

# Host 端单元测试
add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_sync_concurrent test_usermode test_syscall test_shell test_ahci test_ramdisk test_fd_table test_vfs_mount test_ext2 test_ahci_write test_ext2_allocator test_ext2_ops test_ext2_inode_ops test_syscall_ext2 test_shell_write test_cwd_stat test_canvas test_mouse test_event_queue test_window test_window_manager test_terminal test_pipe test_sys_pipe test_shell_redirect
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)

# 统一测试目标：先运行 host 测试，再运行内核测试（自动退出）
# 使用脚本包装，确保 USES_TERMINAL 能正确传递给 QEMU
configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)

add_custom_target(test_all
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_sync_concurrent test_usermode test_syscall test_shell test_ahci test_ramdisk test_fd_table test_vfs_mount test_ext2 test_ahci_write test_ext2_allocator test_ext2_ops test_ext2_inode_ops test_syscall_ext2 test_shell_write test_cwd_stat test_canvas test_mouse test_event_queue test_window test_window_manager test_terminal test_pipe test_sys_pipe test_shell_redirect test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)

# 详细模式运行 host 测试
add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_sync_concurrent test_usermode test_syscall test_shell test_ahci test_ramdisk test_fd_table test_vfs_mount test_ext2 test_ahci_write test_ext2_allocator test_ext2_ops test_ext2_inode_ops test_syscall_ext2 test_shell_write test_cwd_stat test_canvas test_mouse test_event_queue test_window test_window_manager test_terminal test_pipe test_sys_pipe test_shell_redirect
    COMMENT "Running host tests in verbose mode..."
)

# 单个测试快捷方式
add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
