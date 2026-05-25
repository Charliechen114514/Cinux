---
title: 024-shell-2 · Shell
---

# 024-shell-2 Read-through: sys_read 与 Console ANSI 支持

## 概览

本文覆盖 tag 024 中新增的 sys_read 系统调用和 Console ANSI 转义序列解析器的完整代码。sys_read 是 shell 能读取键盘输入的关键——没有它，shell 只能输出不能输入。Console ANSI 支持则让 clear 命令真正能清屏，而不是只发送一串无意义的字节。

## 架构图

```
    用户态 shell                     内核
    ============                     ====
    sys_read(0, &c, 1)
         │
         ▼ SYSCALL (nr=0)
    syscall_entry ──► syscall_dispatch ──► sys_read
                                              │
                                              ▼
                                         Keyboard::poll()
                                              │
                                              ▼
                                         PS/2 ring buffer
                                              │
                                              ▼
                                         KeyEvent {ascii, pressed}
                                              │
                                              ▼ validate + '\r'→'\n'
                                         copy to user buffer
                                              │
                                              ▼
    ←─── return bytes_read ◀────────────────┘

    sys_write(1, "\033[2J\033[H", 7)
         │
         ▼ SYSCALL (nr=1)
    syscall_entry ──► syscall_dispatch ──► sys_write
                                              │
                                              ▼
                                         Console::putc('\x1B')
                                         Console::putc('[')
                                         Console::putc('2')
                                         Console::putc('J')  → clear()
                                         Console::putc('\x1B')
                                         Console::putc('[')
                                         Console::putc('H')  → cursor home
```

## 代码精讲

### sys_read.hpp — 声明

```cpp
#pragma once

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Read data from a file descriptor into a user buffer
 *
 * For fd=0 (stdin), reads keyboard characters from the PS/2 ring buffer.
 * Validates that the user buffer resides below USER_ADDR_MAX.
 *
 * @param fd       File descriptor (only fd=0 is supported)
 * @param buf_virt User virtual address of the destination buffer
 * @param count    Maximum number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count,
                 uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
```

sys_read 的签名与所有 syscall handler 一致——6 个 uint64_t 参数（全部由 syscall_dispatch 传入，分别是 fd、buf_virt、count 和三个占位参数），返回 int64_t。syscall 号在 dispatch 阶段已经用于查表，不传给 handler。多出的参数位用匿名 uint64_t 占位。fd=0 表示 stdin，buf_virt 是用户空间的缓冲区地址，count 是最大读取字节数。

### sys_read.cpp — 实现

```cpp
#include "kernel/syscall/sys_read.hpp"
#include <stdint.h>
#include "kernel/drivers/keyboard/keyboard.hpp"

namespace cinux::syscall {

namespace {
constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;
constexpr uint32_t SPIN_WAIT_ITERS = 1'000'000;
}  // anonymous namespace

int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count,
                 uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) {
        return -1;
    }

    if (fd != 0) {
        return -1;
    }

    auto* buf = reinterpret_cast<char*>(buf_virt);
    uint64_t read_bytes = 0;

    while (read_bytes < count) {
        cinux::drivers::KeyEvent ev;

        if (!cinux::drivers::Keyboard::poll(ev)) {
            if (read_bytes > 0) {
                break;  // Already have some data -- return it immediately
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
                break;  // No input available after waiting
            }
        }

        if (!ev.pressed || ev.ascii == 0) {
            continue;  // Skip release events and non-ASCII keys
        }

        char ch = (ev.ascii == '\r') ? '\n' : ev.ascii;
        buf[read_bytes] = ch;
        read_bytes++;

        if (ch == '\n') {
            break;  // Stop at newline
        }
    }

    return static_cast<int64_t>(read_bytes);
}

}  // namespace cinux::syscall
```

这段代码的读取逻辑分三个层次。最外层是 `while (read_bytes < count)` 循环，控制最多读 count 个字节。中间层是键盘事件的获取——先尝试 poll，如果 buffer 空则进入 spin-wait。最内层是事件过滤——只接受 pressed=true 且 ascii!=0 的事件。

spin-wait 的设计有一个值得注意的优化：如果已经读到了部分数据（read_bytes > 0），即使 buffer 变空也不再等待，直接返回已读到的数据。这避免了"读了一半数据后阻塞在剩余数据上"的问题。

'\r' 到 '\n' 的转换是因为 PS/2 键盘的 Enter 键产生的是 carriage return (0x0D)，而 Unix 传统和 shell 期望的是 newline (0x0A)。这个转换让 shell 的 read_line 函数不需要自己做字符映射。

遇到 '\n' 时立即 break 而不是 continue，实现了逐行读取的语义。shell 的 read_line 每次调用 sys_read(0, &c, 1) 只读一个字符，但 sys_read 本身支持一次读多个字符直到遇到换行。

### Console ANSI 解析 — console.hpp

```cpp
enum class AnsiState : uint8_t {
    Normal,  ///< Not inside an escape sequence
    Esc,     ///< Received ESC (0x1B), expecting '['
    Bracket, ///< Received ESC[, collecting parameters
};

class Console {
    // ... existing members ...
private:
    void handle_ansi_csi(char final_byte);

    AnsiState ansi_state_ = AnsiState::Normal;
    char ansi_params_[16] = {};
    uint8_t ansi_pos_     = 0;
};
```

AnsiState 用 scoped enum（enum class）定义，底层类型是 uint8_t 以节省内存。三个状态对应 ANSI CSI 序列的三个阶段：Normal（等待 ESC）、Esc（收到 ESC，等待 '['）、Bracket（收到 ESC[，收集参数）。ansi_params_ 缓冲区最多存 16 个参数字节，ansi_pos_ 记录当前位置。

### Console ANSI 解析 — console.cpp putc()

```cpp
void Console::putc(char c) {
    if (fb_ == nullptr || font_ == nullptr)
        return;

    // ---- ANSI escape sequence state machine ----
    switch (ansi_state_) {
    case AnsiState::Normal:
        if (c == '\x1B') {
            ansi_state_ = AnsiState::Esc;
            return;
        }
        break;

    case AnsiState::Esc:
        if (c == '[') {
            ansi_state_ = AnsiState::Bracket;
            ansi_pos_   = 0;
            return;
        }
        ansi_state_ = AnsiState::Normal;
        break;

    case AnsiState::Bracket:
        if (ansi_pos_ < sizeof(ansi_params_) - 1 &&
            ((c >= 0x30 && c <= 0x3F) || (c >= 0x20 && c <= 0x2F))) {
            ansi_params_[ansi_pos_++] = c;
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {
            ansi_params_[ansi_pos_] = '\0';
            handle_ansi_csi(c);
            ansi_state_ = AnsiState::Normal;
            return;
        }
        ansi_state_ = AnsiState::Normal;
        return;
    }

    // ---- Normal character processing ----
    switch (c) {
    // ... existing \n, \r, \b handling ...
```

状态机被放在 putc 的最前面，拦截所有输入字符。只有在 Normal 状态下且字符不是 ESC 时，才会 fall through 到下面的正常字符处理逻辑。这保证了 ANSI 序列中的字符不会被当作普通文本渲染到屏幕上。

Bracket 状态的参数收集逻辑按照 VT100 规范实现：参数字节范围是 0x30-0x3F（数字和分号），中间字节范围是 0x20-0x2F。终结字节范围是 0x40-0x7E（字母和部分符号）。不在这个范围内的字节视为 malformed 序列，直接 reset 状态。

### handle_ansi_csi — 命令分发

```cpp
void Console::handle_ansi_csi(char final_byte) {
    switch (final_byte) {
    case 'J': {
        int param = 0;
        for (uint8_t i = 0; i < ansi_pos_; i++) {
            if (ansi_params_[i] >= '0' && ansi_params_[i] <= '9') {
                param = param * 10 + (ansi_params_[i] - '0');
            }
        }
        if (param == 2) {
            clear();
        }
        break;
    }
    case 'H': {
        col_ = 0;
        row_ = 0;
        break;
    }
    default:
        break;
    }
}
```

'J' 命令（Erase in Display）解析参数——ansi_params_ 中存储的是 ASCII 数字字符，需要逐位转换成整数。如果参数是 2，调用 Console::clear() 清除整个屏幕。'H' 命令（Cursor Position）无参数时默认移到 (1,1)，在我们的 0-indexed 坐标系中就是 col_=0, row_=0。其他 CSI 命令（比如 'm' 设置颜色、'A'/'B'/'C'/'D' 移动光标）目前静默忽略——为将来的扩展留了接口。

## 设计决策

### 决策：spin-wait 而非阻塞
**问题**: 键盘输入是异步的，用户可能还没按键
**本项目的做法**: spin-wait 最多 100 万次 pause
**备选方案**: 让出 CPU（Scheduler::yield）等待键盘中断唤醒
**为什么不选备选方案**: 当前调度器不支持阻塞唤醒机制（没有信号量/wait queue）
**如果要扩展**: 实现等待队列后，sys_read 应该在 buffer 空时 yield 并注册回调

### 决策：ANSI 解析放在 Console 而非单独模块
**问题**: ANSI 转义序列应该在哪个层级处理
**本项目的做法**: 直接嵌入 Console::putc() 内部
**备选方案**: 在 sys_write 中预处理 ANSI 序列，然后传给 Console
**为什么不选备选方案**: Console 是唯一的输出目标，不需要抽象终端层

## 扩展方向

- 在 sys_read 中支持更多 fd（文件、管道等）
- 实现非阻塞读取（O_NONBLOCK flag）
- 支持更多 ANSI CSI 命令（光标移动、颜色设置、行擦除）
- 添加 VT100 的 DEC 私有序列支持（比如光标显示/隐藏）

## 参考资料

- Intel SDM: Vol.2A SYSCALL Instruction — 参数传递约定 (RDI/RSI/RDX/R10/R8/R9)
- OSDev Wiki System Calls:
  - URL: https://wiki.osdev.org/System_Calls
- VT100 User Guide: ANSI escape sequences 定义
  - URL: https://vt100.net/docs/vt100-ug/chapter3.html
