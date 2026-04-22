/**
 * @file kernel/syscall/sys_read.cpp
 * @brief sys_read handler implementation
 *
 * For fd=0 (stdin): reads keyboard input from the PS/2 ring buffer.
 * For other fds: reads through VFS (fd -> File -> Inode -> ops -> read).
 */

#include "kernel/syscall/sys_read.hpp"

#include <stdint.h>

#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/file.hpp"

namespace cinux::syscall {

namespace {

/// Maximum iterations to spin-wait for a key before returning 0
constexpr uint32_t SPIN_WAIT_ITERS = 1'000'000;

}  // anonymous namespace

int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count,
                 uint64_t, uint64_t, uint64_t) {
    if (buf_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    // fd=0 (stdin): legacy keyboard read path
    if (fd == 0) {
        auto* buf = reinterpret_cast<char*>(buf_virt);
        uint64_t read_bytes = 0;

        while (read_bytes < count) {
            cinux::drivers::KeyEvent ev;

            if (!cinux::drivers::Keyboard::poll(ev)) {
                if (read_bytes > 0) {
                    break;
                }

                bool got_key = false;
                for (uint32_t i = 0; i < SPIN_WAIT_ITERS; i++) {
                    __asm__ volatile("pause");
                    if (cinux::drivers::Keyboard::poll(ev)) {
                        got_key = true;
                        break;
                    }
                }

                if (!got_key) {
                    break;
                }
            }

            if (!ev.pressed || ev.ascii == 0) {
                continue;
            }

            char ch = (ev.ascii == '\r') ? '\n' : ev.ascii;

            buf[read_bytes] = ch;
            read_bytes++;

            if (ch == '\n') {
                break;
            }
        }

        return static_cast<int64_t>(read_bytes);
    }

    // fd > 0: VFS-based read
    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode == nullptr || file->inode->ops == nullptr) {
        return -1;
    }

    if (file->inode->ops->read == nullptr) {
        return -1;
    }

    auto* buf = reinterpret_cast<void*>(buf_virt);
    int64_t result = file->inode->ops->read(file->inode, file->offset, buf, count);

    if (result > 0) {
        file->offset += static_cast<uint64_t>(result);
    }

    return result;
}

}  // namespace cinux::syscall
