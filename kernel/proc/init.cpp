#include "kernel/proc/init.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/per_cpu.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

#ifdef CINUX_GUI
#    include "kernel/gui/gui_init.hpp"
#endif

namespace cinux::proc {

#ifdef CINUX_GUI
namespace {

void gui_worker_thread() {
    cinux::lib::kprintf("[GUI] Worker thread started\n");
    while (true) {
        cinux::gui::gui_process_pending();
        Scheduler::yield();
    }
}

}  // anonymous namespace
#endif

void kernel_init_thread() {
    auto* self = Scheduler::current();
    cinux::lib::kprintf("[INIT] kernel_init started tid=%u\n", self ? self->tid : 0);

    cinux::lib::kprintf("[INIT] ===== Milestone 028: ext2 Filesystem =====\n");
    static cinux::fs::Ext2 ext2(cinux::drivers::ahci::AHCI::instance(), 1);
    if (!ext2.mount()) {
        cinux::lib::kprintf("[INIT] ext2 mount failed!\n");
    }

    cinux::lib::kprintf("[INIT] ===== Milestone 027: VFS =====\n");
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", &ext2);
    cinux::lib::kprintf("[VFS] ext2 mounted at /\n");

#ifdef CINUX_GUI
    // Pipe creation is now handled per-terminal inside create_shell_terminal().
    // No global pipe setup needed here -- each Shell desktop icon click
    // dynamically creates its own pipe pair, forks a shell, and binds them.

    cinux::lib::kprintf("[INIT] ===== Milestone 035: Multi-Terminal =====\n");

    // Start the GUI: mouse init, desktop icons, PIT tick callback
    cinux::gui::gui_start();

    // Launch GUI worker thread to handle deferred work (fork/execve)
    // outside of PIT interrupt context
    auto* gui_task = TaskBuilder().set_entry(gui_worker_thread).set_name("gui_worker").build();
    if (gui_task != nullptr) {
        Scheduler::add_task(gui_task);
        cinux::lib::kprintf("[INIT] GUI worker thread launched\n");
    }
#else
    // Non-GUI mode: fork and execve the shell directly.
    // Uses legacy sys_read (keyboard polling) for stdin and
    // legacy sys_write (kprintf serial+console) for stdout.
    cinux::lib::kprintf("[INIT] Launching shell (non-GUI mode)...\n");
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid == 0) {
        __asm__ volatile("cli");

        auto* task       = cinux::proc::Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();

        const char* path   = "/bin/sh";
        const char* argv[] = {path, nullptr};
        const char* envp[] = {nullptr};

        auto result = cinux::proc::execve(path, argv, envp);
        if (result != cinux::proc::ExecveResult::Ok) {
            cinux::lib::kprintf("[INIT] execve(%s) failed: %d\n", path, static_cast<int>(result));
            cinux::proc::Scheduler::exit_current();
        }

        // Set up user stack
        uint64_t           entry = task->ctx.rip;
        constexpr uint64_t kUserPageFlags =
            cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_USER;
        uint64_t stack_base =
            cinux::arch::USER_STACK_TOP - cinux::arch::USER_STACK_PAGES * cinux::arch::PAGE_SIZE;

        for (uint64_t i = 0; i < cinux::arch::USER_STACK_PAGES; i++) {
            uint64_t phys = cinux::mm::g_pmm.alloc_page();
            if (phys == 0) {
                cinux::lib::kprintf("[INIT] user stack alloc failed\n");
                cinux::proc::Scheduler::exit_current();
            }
            uint64_t virt = stack_base + i * cinux::arch::PAGE_SIZE;
            if (!task->addr_space->map(virt, phys, kUserPageFlags)) {
                cinux::lib::kprintf("[INIT] user stack map failed at %p\n",
                                    reinterpret_cast<void*>(virt));
                cinux::proc::Scheduler::exit_current();
            }
        }

        cinux::lib::kprintf("[INIT] Shell jumping to user mode: entry=%p\n",
                            reinterpret_cast<void*>(entry));

        task->addr_space->activate();
        cinux::proc::g_per_cpu.update_syscall_stack(task->kernel_stack_top);
        jump_to_usermode(entry, cinux::arch::USER_STACK_TOP - cinux::arch::USER_ABI_RSP_OFFSET, 0);
        cinux::proc::Scheduler::exit_current();
    } else if (child_pid > 0) {
        cinux::lib::kprintf("[INIT] Shell spawned pid=%d\n", child_pid);
    } else {
        cinux::lib::kprintf("[INIT] fork() failed: %d\n", child_pid);
    }
#endif

    Scheduler::exit_current();
}

}  // namespace cinux::proc
