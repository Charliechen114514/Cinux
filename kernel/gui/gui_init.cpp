/**
 * @file kernel/gui/gui_init.cpp
 * @brief GUI subsystem initialisation implementation
 *
 * Encapsulates all GUI setup (mouse driver, window manager, PIT
 * tick callback) behind the gui_init() / gui_start() interface so
 * that kernel_main and kernel_init_thread remain GUI-agnostic.
 */

#include "gui_init.hpp"

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/mouse.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/gui/desktop_icon.hpp"
#include "kernel/gui/event.hpp"
#include "kernel/gui/icon.hpp"
#include "kernel/gui/terminal.hpp"
#include "kernel/gui/window_manager.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/lib/atomic.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/per_cpu.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::gui {

// ============================================================
// Module-internal state
// ============================================================

namespace {
cinux::drivers::Canvas*  g_screen = nullptr;
cinux::drivers::PSFFont* g_font   = nullptr;

/// Counter for generating unique terminal titles
uint32_t g_terminal_counter = 0;

/// Deferred work queue: ISR enqueues, gui_worker thread drains.
cinux::lib::Atomic<IconAction> g_pending_action{IconAction::None};

}  // anonymous namespace

// ============================================================
// gui_init() -- one-time GUI setup from kernel_main
// ============================================================

void gui_init(cinux::drivers::Canvas& screen, cinux::drivers::PSFFont& font) {
    cinux::lib::kprintf("[GUI] Initialising GUI subsystem...\n");

    // Store pointers for later use in gui_start()
    g_screen = &screen;
    g_font   = &font;

    // Initialise the window manager with the screen canvas and font
    WindowManager::instance().init(&screen, &font);

    // Draw the GUI demo: dark background + random-coloured rectangles + title
    screen.clear(0x001A1A2E);
    uint32_t rng_state = 12345;
    auto     lcg_next  = [&rng_state]() {
        rng_state = rng_state * 1103515245u + 12345u;
        return (rng_state >> 16) & 0x7FFF;
    };
    for (int i = 0; i < 10; i++) {
        uint32_t x     = lcg_next() % (screen.width() - 100);
        uint32_t y     = lcg_next() % (screen.height() - 60);
        uint32_t w     = 40 + (lcg_next() % 120);
        uint32_t h     = 30 + (lcg_next() % 80);
        uint32_t r     = 0x40 + (lcg_next() % 0xC0);
        uint32_t g     = 0x40 + (lcg_next() % 0xC0);
        uint32_t b     = 0x40 + (lcg_next() % 0xC0);
        uint32_t color = (r << 16) | (g << 8) | b;
        screen.draw_rect(x, y, w, h, color);
    }
    const char* title  = "Cinux GUI";
    uint32_t    text_w = 9 * font.width();
    uint32_t    text_x = (screen.width() - text_w) / 2;
    screen.draw_text(text_x, 10, title, 0x00FFFFFF, font);
    screen.flip();

    cinux::lib::kprintf("[GUI] Demo rendered to framebuffer.\n");
}

// ============================================================
// Internal helper: create a shell terminal window
// ============================================================

namespace {

void create_shell_terminal() {
    auto& wm = WindowManager::instance();

    // Generate a unique title for this terminal instance
    g_terminal_counter++;
    char title_buf[64];
    strcpy(title_buf, "Shell #");
    utoa(title_buf + strlen(title_buf), g_terminal_counter);

    // Calculate terminal dimensions
    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400

    // Centre the terminal on screen if possible
    uint32_t term_x = 80;
    uint32_t term_y = 60;

    if (g_screen != nullptr) {
        uint32_t sw = g_screen->width();
        uint32_t sh = g_screen->height();
        if (term_w + 80 < sw) {
            term_x = (sw - term_w) / 2;
        }
        if (term_h + 60 < sh) {
            term_y = (sh - term_h) / 2;
        }
    }

    auto* term = new Terminal(term_x, term_y, title_buf);
    term->set_font(g_font);

    // --- Create per-terminal pipes ---

    // stdin pipe: Terminal.on_key() writes -> shell reads from fd 0
    auto* stdin_pipe       = new cinux::ipc::Pipe();
    auto* stdin_read_ops   = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops  = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    // stdout pipe: shell writes to fd 1 -> Terminal.poll_output() reads
    auto* stdout_pipe        = new cinux::ipc::Pipe();
    auto* stdout_write_ops   = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops  = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    // Bind pipe pointers to the terminal (kernel-side direct access)
    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);

    // --- Fork and exec shell ---
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid > 0) {
        // Parent: record the shell PID
        term->set_shell_pid(child_pid);
        cinux::lib::kprintf("[GUI] Terminal '%s': shell spawned pid=%d\n", title_buf, child_pid);
    } else if (child_pid == 0) {
        // ---- Child path ----
        // The parent (gui_worker) is a kernel thread with no address space.
        // Create a fresh AddressSpace so execve() can map the ELF segments.
        __asm__ volatile("cli");

        auto* task       = cinux::proc::Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();

        // Set up a private FDTable so this shell reads/writes through
        // its own terminal's pipes -- independent of the global table.
        task->fd_table = new cinux::fs::FDTable();
        task->fd_table->set(0,
                            new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY));
        task->fd_table->set(
            1, new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY));

        // execve the shell
        const char* path   = "/bin/sh";
        const char* argv[] = {path, nullptr};
        const char* envp[] = {nullptr};

        auto result = cinux::proc::execve(path, argv, envp);
        if (result != cinux::proc::ExecveResult::Ok) {
            cinux::lib::kprintf("[GUI] execve(%s) failed: %d\n", path, static_cast<int>(result));
            cinux::proc::Scheduler::exit_current();
        }

        // execve succeeded: set up user stack
        uint64_t entry = task->ctx.rip;

        constexpr uint64_t kUserPageFlags =
            cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_USER;
        uint64_t stack_base =
            cinux::arch::USER_STACK_TOP - cinux::arch::USER_STACK_PAGES * cinux::arch::PAGE_SIZE;

        for (uint64_t i = 0; i < cinux::arch::USER_STACK_PAGES; i++) {
            uint64_t phys = cinux::mm::g_pmm.alloc_page();
            if (phys == 0) {
                cinux::lib::kprintf("[GUI] user stack alloc failed\n");
                cinux::proc::Scheduler::exit_current();
            }
            uint64_t virt = stack_base + i * cinux::arch::PAGE_SIZE;
            if (!task->addr_space->map(virt, phys, kUserPageFlags)) {
                cinux::lib::kprintf("[GUI] user stack map failed at %p\n",
                                    reinterpret_cast<void*>(virt));
                cinux::proc::Scheduler::exit_current();
            }
        }

        cinux::lib::kprintf("[GUI] Shell child jumping to user mode: entry=%p\n",
                            reinterpret_cast<void*>(entry));

        // Activate the child's address space -- now fully populated
        task->addr_space->activate();

        // Update the per-CPU syscall kernel stack so syscall_entry
        // loads the child's kernel stack (not the parent's or the
        // boot task's).
        cinux::proc::g_per_cpu.update_syscall_stack(task->kernel_stack_top);

        // Do NOT sti here.  SYSRETQ restores R11 into RFLAGS, and R11
        // already has IF=1 (set in jump_to_usermode), so interrupts
        // are enabled atomically upon entering Ring 3.  An explicit
        // sti before SYSRETQ opens a window where the PIT fires on
        // the child's CR3, causing gui_tick_callback to composite
        // with incomplete identity mappings -- demand-paging zero
        // pages over the framebuffer MMIO region.

        jump_to_usermode(entry, cinux::arch::USER_STACK_TOP - cinux::arch::USER_ABI_RSP_OFFSET, 0);

        cinux::proc::Scheduler::exit_current();
    } else {
        cinux::lib::kprintf("[GUI] fork() failed: %d\n", child_pid);
        // Clean up all allocated resources on fork failure
        delete stdin_read_ops;
        delete stdin_read_inode;
        delete stdout_write_ops;
        delete stdout_write_inode;
        delete stdin_pipe;
        delete stdout_pipe;
        delete term;
        return;
    }

    wm.add_window(term);
    cinux::lib::kprintf("[GUI] Terminal '%s' created with pipes stdin=%p stdout=%p\n", title_buf,
                        reinterpret_cast<void*>(stdin_pipe), reinterpret_cast<void*>(stdout_pipe));
}

}  // anonymous namespace

// ============================================================
// PIT tick callback: process events + composite
// ============================================================

namespace {

/**
 * @brief Called on every PIT tick to drain input and refresh the screen
 *
 * @param ctx  Unused context pointer
 */
void gui_tick_callback(void* /*ctx*/) {
    using cinux::drivers::Mouse;
    using cinux::gui::Event;
    using cinux::gui::EventType;

    static uint32_t tick_count   = 0;
    static uint32_t total_events = 0;
    static uint32_t mouse_events = 0;
    tick_count++;

    auto& wm = WindowManager::instance();
    auto& eq = Mouse::event_queue();

    // Drain all pending events from the queue
    Event ev;
    while (eq.dequeue(ev)) {
        total_events++;

        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            mouse_events++;
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // Check if a desktop icon was clicked -- enqueue for deferred processing
    IconAction action = wm.consume_pending_icon_action();
    if (action != IconAction::None) {
        g_pending_action.store(action, cinux::lib::MemoryOrder::Release);
    }

    // Poll all terminal windows for shell output (not just the focused one)
    // so that multiple concurrent shell sessions all update their displays.
    for (uint32_t i = 0; i < wm.window_count(); i++) {
        auto* win = wm.window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<Terminal*>(win);
            term->poll_output();
            term->render_to_canvas();
        }
    }

    // Composite all windows onto the screen
    wm.composite();
}

}  // anonymous namespace

// ============================================================
// gui_start() -- activate the WM tick loop from kernel_init_thread
// ============================================================

void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop =====\n");

    // Initialise PS/2 mouse driver
    cinux::drivers::Mouse::init();

    // Configure mouse screen bounds to match the canvas
    if (g_screen != nullptr) {
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());
    }

    // Register desktop icons on the desktop
    auto& wm = WindowManager::instance();

    DesktopIcon shell_icon{
        .x      = 40,
        .y      = 40,
        .bitmap = icons::data::k_shell_icon.data(),
        .label  = "Shell",
        .width  = icons::ICON_SIZE,
        .height = icons::ICON_SIZE,
        .action = IconAction::OpenShell,
    };
    wm.add_desktop_icon(shell_icon);

    DesktopIcon calc_icon{
        .x      = 40,
        .y      = 120,
        .bitmap = icons::data::k_calc_icon.data(),
        .label  = "Calculator",
        .width  = icons::ICON_SIZE,
        .height = icons::ICON_SIZE,
        .action = IconAction::OpenCalculator,
    };
    wm.add_desktop_icon(calc_icon);

    cinux::lib::kprintf("[GUI] Desktop icons registered: Shell, Calculator.\n");

    // Register the GUI tick callback for event processing + compositing
    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
    cinux::lib::kprintf("[GUI] GUI tick callback registered on PIT.\n");
}

// ============================================================
// gui_process_pending() -- drain deferred work from ISR
// ============================================================

void gui_process_pending() {
    IconAction action =
        g_pending_action.exchange(IconAction::None, cinux::lib::MemoryOrder::AcqRel);
    if (action == IconAction::OpenShell) {
        create_shell_terminal();
    }
}

}  // namespace cinux::gui
