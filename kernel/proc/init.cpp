#include "kernel/proc/init.hpp"

#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

#ifdef CINUX_GUI
#include "kernel/gui/gui_init.hpp"
#endif

namespace cinux::proc {

void kernel_init_thread() {
    auto* self = Scheduler::current();
    cinux::lib::kprintf("[INIT] kernel_init started tid=%u\n",
                        self ? self->tid : 0);

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
    // Start the GUI: mouse init, test windows, PIT tick callback
    cinux::gui::gui_start();
#endif

    cinux::lib::kprintf("[INIT] ===== Milestone 023: Syscall from Ring 3 =====\n");
    cinux::arch::launch_first_user();

    cinux::lib::kprintf("[INIT] launch_first_user returned, exiting.\n");
    Scheduler::exit_current();
}

}  // namespace cinux::proc
