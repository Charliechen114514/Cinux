/**
 * @file kernel/mini/lib/private/vkprintf_impl.h
 * @brief Variadic formatted output engine (template, hardware-independent)
 *
 * Contains the core format parser used by both kprintf() and kdebugf().
 * Decoupled from any output device so that host-side unit tests can
 * supply a mock OutputFn without linking the serial / IO driver.
 *
 * Supported specifiers:  %%  %c  %s  %d  %u  %x  %X  %p  %b
 * Width modifiers:       %Nd   (right-align, space-pad)
 *                        %0Nd  (right-align, zero-pad)
 *                        %-Ns  (left-align, space-pad)
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "format.h"

namespace cinux::mini::lib::detail {

// ============================================================
// Generic formatted output engine
// ============================================================
// OutputFn: void(char) - functor/lambda to output a single character
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];

    while (*format != '\0') {
        if (*format != '%') {
            putc(*format++);
            continue;
        }

        // consume '%'
        format++;

        // check for left-align flag
        bool left_align = false;
        if (*format == '-') {
            left_align = true;
            format++;
        }

        // check for zero-pad flag
        bool zero_pad = false;
        if (*format == '0') {
            zero_pad = true;
            format++;
        }

        // parse width
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        char type = *format++;

        switch (type) {
        case '%':
            putc('%');
            break;

        case 'c':
            putc(static_cast<char>(va_arg(args, int)));
            break;

        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) {
                s = "(null)";
            }

            // measure string length
            int slen = 0;
            while (s[slen] != '\0') {
                slen++;
            }

            if (left_align) {
                // print string first, then pad
                for (int i = 0; i < slen; i++) {
                    putc(s[i]);
                }
                for (int i = slen; i < width; i++) {
                    putc(' ');
                }
            } else {
                // pad first, then string
                for (int i = slen; i < width; i++) {
                    putc(' ');
                }
                for (int i = 0; i < slen; i++) {
                    putc(s[i]);
                }
            }
            break;
        }

        case 'd': {
            int len =
                format_decimal(static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
            // determine if buffer starts with a sign character
            bool has_sign   = (len > 0 && buffer[0] == '-');
            int  digits_len = has_sign ? len - 1 : len;

            if (!left_align && zero_pad && has_sign) {
                // sign first, then zero-pad, then digits
                putc('-');
                for (int i = digits_len; i < width - 1; i++) {
                    putc('0');
                }
                for (int i = 1; i < len; i++) {
                    putc(buffer[i]);
                }
            } else if (!left_align) {
                // space-pad before entire content
                char pad = zero_pad ? '0' : ' ';
                for (int i = len; i < width; i++) {
                    putc(pad);
                }
                for (int i = 0; i < len; i++) {
                    putc(buffer[i]);
                }
            } else {
                // left-align: content first, then spaces
                for (int i = 0; i < len; i++) {
                    putc(buffer[i]);
                }
                for (int i = len; i < width; i++) {
                    putc(' ');
                }
            }
            break;
        }

        case 'u': {
            int  len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)), buffer,
                                      sizeof(buffer));
            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc(' ');
                }
            }
            break;
        }

        case 'x': {
            int  len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc(' ');
                }
            }
            break;
        }

        case 'X': {
            int  len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc(' ');
                }
            }
            break;
        }

        case 'p': {
            // always output "0x" + 16-digit zero-padded uppercase hex
            putc('0');
            putc('x');
            int len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            // pad to 16 digits
            for (int i = len; i < 16; i++) {
                putc('0');
            }
            for (int i = 0; i < len; i++) {
                putc(buffer[i]);
            }
            break;
        }

        case 'b': {
            int  len = format_binary(va_arg(args, uint64_t), buffer, sizeof(buffer));
            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc(' ');
                }
            }
            break;
        }

        default:
            putc('%');
            putc(type);
            break;
        }
    }
}

}  // namespace cinux::mini::lib::detail
