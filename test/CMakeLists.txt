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
# 测试辅助目标
# ============================================================

# Host 端单元测试
add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_usermode
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

add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_usermode test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)

# 详细模式运行 host 测试
add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test_ata test_elf_loader test_big_kernel_loader test_kprintf test_pic test_pit test_font test_console test_framebuffer test_keyboard test_pmm test_vmm test_heap test_address_space test_scheduler test_sync test_usermode
    COMMENT "Running host tests in verbose mode..."
)

# 单个测试快捷方式
add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
