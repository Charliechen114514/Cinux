/**
 * @file kernel/lib/kprintf.cpp
 * @brief Kernel formatted print implementation (serial output)
 *
 * Internal structure:
 *   1. Number formatting helpers (format_decimal, format_hex)
 *      extracted as file-local (anonymous-namespace) functions.
 *   2. A generic vkprintf_impl() template that accepts any
 *      character-output functor (so the same formatter works
 *      for serial, debug console, or a mock buffer in tests).
 *   3. Public kprintf / kvprintf / kpanic wrappers.
 */

#include "kernel/lib/kprintf.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/drivers/serial.hpp"

namespace {

using cinux::drivers::Serial;
using cinux::drivers::SERIAL_COM1;

// ============================================================
// File-local formatting helpers
// ============================================================

/**
 * @brief Format a signed 64-bit integer as decimal
 *
 * @param value        The number to format
 * @param buffer       Output buffer (at least 24 bytes)
 * @param buffer_size  Capacity of buffer
 * @return Number of characters written (excluding NUL)
 */
static int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == (-9223372036854775807LL - 1)) {
            // INT64_MIN special case
            const char* min_str = "-9223372036854775808";
            int len = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1) {
                buffer[idx++] = min_str[len++];
            }
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}

/**
 * @brief Format an unsigned 64-bit integer as hexadecimal
 *
 * @param value        The number to format
 * @param buffer       Output buffer (at least 20 bytes)
 * @param buffer_size  Capacity of buffer
 * @param lowercase    true = 'a'-'f', false = 'A'-'F'
 * @return Number of characters written (excluding NUL)
 */
static int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char  tmp[20];
    int   tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}

// ============================================================
// Generic formatted output engine
// ============================================================
// OutputFn: callable(char) -- invoked for each output character

template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }

        // Consume '%'
        fmt++;

        // Parse optional zero-pad flag
        bool zero_pad = false;
        int  width    = 0;

        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // Parse optional width
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char type = *fmt++;
        int  len  = 0;

        switch (type) {
        case '%':
            putc_fn('%');
            break;

        case 'c':
            putc_fn(static_cast<char>(va_arg(args, int)));
            break;

        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) s = "(null)";
            while (*s) putc_fn(*s++);
            break;
        }

        case 'd':
            len = format_decimal(static_cast<int64_t>(va_arg(args, int)),
                                 buffer, sizeof(buffer));
            goto do_padding;

        case 'u':
            len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)),
                                 buffer, sizeof(buffer));
            goto do_padding;

        case 'x':
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
            goto do_padding;

        case 'X':
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            goto do_padding;

        case 'p':
            // Checklist: %p outputs "0x" + 16-digit hex
            putc_fn('0');
            putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            // Pad to 16 digits with leading zeros
            for (int i = len; i < 16; i++) {
                putc_fn('0');
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;

        do_padding:
            if (len < width) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = width - len; i > 0; i--) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;

        default:
            putc_fn('%');
            putc_fn(type);
            break;
        }
    }
}

// ============================================================
// Global serial instance (singleton for big kernel)
// ============================================================

static Serial g_serial(SERIAL_COM1);

}  // anonymous namespace

namespace cinux::lib {

// ============================================================
// kprintf_init -- one-time serial port setup for kprintf
// ============================================================

void kprintf_init() {
    g_serial.init();
}

// ============================================================
// kprintf -- variadic serial print
// ============================================================

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}

// ============================================================
// kvprintf -- va_list serial print
// ============================================================

void kvprintf(const char* fmt, va_list args) {
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
}

// ============================================================
// kpanic -- print + halt
// ============================================================

void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);

    // Halt forever
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // namespace cinux::lib
