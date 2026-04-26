/**
 * @file kernel/test/main_test.cpp
 * @brief Big kernel test entry point
 *
 * Replaces the production kernel_main with a test harness that initializes
 * GDT/IDT, runs the GDT/IDT test suite, and exits via QEMU isa-debug-exit.
 *
 * Exit codes:
 *   0 = all tests passed (QEMU exits with code 1 via isa-debug-exit)
 *   1 = some tests failed (QEMU exits with code 3 via isa-debug-exit)
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

extern "C" {
void run_gdt_idt_tests();
void run_pic_pit_tests();
void run_video_tests();
void run_keyboard_tests();
void run_pmm_tests();
void run_vmm_tests();
void run_heap_tests();
void run_heap_lock_stress_tests();
void run_address_space_tests();
void run_scheduler_tests();
void run_sync_tests();
void run_usermode_tests();
void run_syscall_tests();
void run_shell_tests();
void run_ahci_tests();
void run_ramdisk_tests();
void run_vfs_syscall_tests();
void run_ext2_tests();
void run_ahci_write_tests();
void run_ext2_allocator_tests();
void run_ext2_ops_tests();
void run_ext2_inode_ops_tests();
void run_syscall_ext2_tests();
void run_shell_write_tests();
void run_cwd_stat_tests();
void run_sync_concurrent_tests();
void run_canvas_tests();
void run_mouse_event_tests();
void run_window_tests();
void run_window_manager_tests();
void run_gui_integration_tests();
void run_bitmap_icon_tests();
void run_desktop_tests();
void run_terminal_tests();
void run_pipe_tests();
void run_sys_pipe_tests();
void run_terminal_shell_tests();
void run_fork_exec_tests();
void run_multi_terminal_tests();
void run_kprintf_format_tests();
}

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

extern "C" void kernel_main() {
    // Step 1: Initialise serial port for test output
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[TEST] Big Kernel Test Suite starting...\n");

    // Step 2: Initialise GDT (must come before IDT)
    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[TEST] GDT loaded.\n");

    // Step 3: Initialise IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[TEST] IDT loaded.\n");

    // kprintf format tests run early — only need serial + kprintf
    run_kprintf_format_tests();

    // Step 4: Run test suites (hardware only)
    run_gdt_idt_tests();
    run_pic_pit_tests();
    run_keyboard_tests();

    // PMM tests: initialise with real BootInfo, then run tests
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    cinux::mm::g_pmm.init(*boot_info);
    run_pmm_tests();

    // VMM tests: initialise VMM after PMM, then run tests
    cinux::mm::g_vmm.init();
    run_vmm_tests();

    // Video tests require VMM (framebuffer maps via map_2mb)
    run_video_tests();

    // Heap tests: initialise Heap after VMM, then run tests
    constexpr uint64_t HEAP_VIRT_BASE = cinux::arch::KMEM_HEAP_BASE;
    constexpr uint64_t HEAP_INIT_SIZE = 64 * 1024;  // 64 KB
    cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INIT_SIZE);
    run_heap_tests();
    run_heap_lock_stress_tests();

    run_pipe_tests();
    run_sys_pipe_tests();
    run_canvas_tests();
    run_mouse_event_tests();
    run_window_tests();
    run_window_manager_tests();
    run_gui_integration_tests();
    run_bitmap_icon_tests();
    run_desktop_tests();
    run_terminal_tests();
#ifdef CINUX_GUI
    run_terminal_shell_tests();
#endif
    cinux::mm::AddressSpace::init_kernel();
    run_address_space_tests();

    run_scheduler_tests();
    run_sync_tests();
    run_sync_concurrent_tests();

    cinux::arch::usermode_init();
    run_usermode_tests();

    cinux::arch::syscall_init();
    run_syscall_tests();

    run_fork_exec_tests();
#ifdef CINUX_GUI
    // Multi-terminal tests (035): multiple concurrent terminals with
    // independent pipes, destructor cleanup, WM iteration, tick callback
    run_multi_terminal_tests();
#endif
    // Shell tests (024): verifies kernel-side infrastructure for user shell
    run_shell_tests();

    // AHCI tests (025): requires PMM and VMM for BAR5 mapping and DMA buffers
    run_ahci_tests();

    // Ramdisk tests (026): verifies ustar parsing of embedded initrd
    run_ramdisk_tests();

    // VFS syscall integration tests (027): sys_open/read/write/close via VFS
    run_vfs_syscall_tests();

    // Ext2 filesystem tests (028): mount, lookup, read, readdir, VFS integration
    run_ext2_tests();

    // AHCI write + ext2 write_block tests (028b): write round-trip, write_block
    run_ahci_write_tests();

    // Ext2 allocator tests (028b): alloc_block, free_block, alloc_inode, free_inode
    run_ext2_allocator_tests();

    // Ext2 write/create/mkdir/unlink tests (028b)
    run_ext2_ops_tests();

    // Ext2 InodeOps virtual class tests (028b)
    run_ext2_inode_ops_tests();

    // Syscall ext2 integration tests (028b): sys_creat/mkdir/unlink/rmdir
    run_syscall_ext2_tests();

    // Shell write command tests (028b): touch/mkdir/rm/rmdir/echo redirect
    run_shell_write_tests();

    // CWD/stat tests (028c): chdir/getcwd/stat/fstat/path canonicalize
    run_cwd_stat_tests();

    // Step 5: Report and exit
    int exit_code = (test::get_total_failed() > 0) ? 1 : 0;

    if (exit_code != 0) {
        cinux::lib::kprintf("\n[TEST] TESTS FAILED (exit code %d)\n", exit_code);
    } else {
        cinux::lib::kprintf("\n[TEST] ALL TESTS PASSED (exit code %d)\n", exit_code);
    }

    // Exit via QEMU isa-debug-exit device (port 0xf4)
    __asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));

    // Fallback halt if isa-debug-exit is not available
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
