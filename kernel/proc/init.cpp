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
#include "kernel/gui/terminal.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
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
    // Start the GUI: mouse init, terminal window, PIT tick callback
    auto* term = cinux::gui::gui_start();

    // Create stdin pipe: Terminal on_key -> shell sys_read(0)
    auto* stdin_pipe = new cinux::ipc::Pipe();
    auto* stdin_read_ops = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    // Create stdout pipe: shell sys_write(1) -> Terminal poll_output
    auto* stdout_pipe = new cinux::ipc::Pipe();
    auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    // Bind fd 0 (stdin) to stdin_pipe read end
    auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
    cinux::fs::g_global_fd_table().set(0, stdin_file);

    // Bind fd 1 (stdout) to stdout_pipe write end
    auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
    cinux::fs::g_global_fd_table().set(1, stdout_file);

    // Connect pipes to the Terminal
    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);

    cinux::lib::kprintf("[INIT] Terminal-shell pipes connected: stdin_pipe=%p stdout_pipe=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));
#endif

    cinux::lib::kprintf("[INIT] ===== Milestone 023: Syscall from Ring 3 =====\n");
    cinux::arch::launch_first_user();

    cinux::lib::kprintf("[INIT] launch_first_user returned, exiting.\n");
    Scheduler::exit_current();
}

}  // namespace cinux::proc
