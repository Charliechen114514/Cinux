/**
 * @file kernel/lib/private/vkprintf_impl.hpp
 * @brief Variadic formatted output engine (template, hardware-independent)
 *
 * Contains the core format parser used by kprintf() and kvprintf().
 * Decoupled from any output device so that host-side unit tests can
 * supply a mock OutputFn without linking the serial / IO driver.
 *
 * Supported specifiers:  %%  %c  %s  %d  %u  %x  %X  %p
 * Width modifiers:       %Nd   (right-align, space-pad)
 *                        %0Nd  (right-align, zero-pad)
 *                        %-Nd  (left-align, space-pad for numbers)
 *                        %-Ns  (left-align, space-pad for strings)
 *
 * @note This file is internal -- do not include from public headers.
 *       The big kernel's kprintf.cpp includes this, and host-side
 *       test_kprintf.cpp includes this too.
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

namespace cinux::lib::detail {

// ============================================================
// Internal number formatting helpers
// ============================================================

/**
 * @brief Format a signed 64-bit integer as decimal
 *
 * @param value        The number to format
 * @param buffer       Output buffer (at least 24 bytes)
 * @param buffer_size  Capacity of buffer
 * @return Number of characters written (excluding NUL)
 */
inline int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) {
        return 0;
    }

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == static_cast<int64_t>(0x8000000000000000ULL)) {
            // INT64_MIN special case -- cannot negate
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
inline int format_hex(uint64_t value, char* buffer, int buffer_size,
                      bool lowercase) {
    if (buffer_size < 1) {
        return 0;
    }

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

        // Parse optional left-align flag '-'
        bool left_align = false;
        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }

        // Parse optional zero-pad flag '0'
        bool zero_pad = false;
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // Parse optional width
        int width = 0;
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
            if (s == nullptr) {
                s = "(null)";
            }

            // Measure string length
            int slen = 0;
            while (s[slen] != '\0') {
                slen++;
            }

            if (left_align) {
                // Print string first, then pad with spaces
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
            } else {
                // Pad first, then string
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
            }
            break;
        }

        case 'd': {
            len = format_decimal(static_cast<int64_t>(va_arg(args, int)),
                                 buffer, sizeof(buffer));

            // Determine if the formatted string starts with a sign
            bool has_sign = (len > 0 && buffer[0] == '-');
            int digits_len = has_sign ? len - 1 : len;

            if (!left_align && zero_pad && has_sign) {
                // Sign first, then zero-pad, then digits
                putc_fn('-');
                for (int i = digits_len; i < width - 1; i++) {
                    putc_fn('0');
                }
                for (int i = 1; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else if (!left_align) {
                // Right-align: pad before entire content
                char pad = zero_pad ? '0' : ' ';
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else {
                // Left-align: content first, then spaces
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'u': {
            len = format_decimal(
                static_cast<int64_t>(va_arg(args, unsigned int)),
                buffer, sizeof(buffer));

            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'x': {
            len = format_hex(va_arg(args, uint64_t), buffer,
                             sizeof(buffer), true);

            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'X': {
            len = format_hex(va_arg(args, uint64_t), buffer,
                             sizeof(buffer), false);

            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'p': {
            // Always output "0x" + 16-digit zero-padded uppercase hex
            putc_fn('0');
            putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer,
                             sizeof(buffer), false);

            // Pad to 16 digits with leading zeros
            for (int i = len; i < 16; i++) {
                putc_fn('0');
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;
        }

        default:
            putc_fn('%');
            putc_fn(type);
            break;
        }
    }
}

}  // namespace cinux::lib::detail
