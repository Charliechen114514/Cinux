/**
 * @file user/libc/printf.cpp
 * @brief Minimal user-space printf backed by sys_write
 *
 * Supports: %c, %s, %d, %u, %x, %p, %%
 * All output goes to fd 1 (stdout).
 */

#include "printf.hpp"

#include <cstdarg>

#include "syscall.h"

namespace cinux::user {

namespace {

constexpr size_t PRINTF_BUF_SIZE = 256;

void flush_buf(char* buf, size_t& pos) {
    if (pos > 0) {
        sys_write(1, buf, pos);
        pos = 0;
    }
}

void putc_buf(char* buf, size_t& pos, char c) {
    buf[pos++] = c;
    if (pos >= PRINTF_BUF_SIZE) {
        flush_buf(buf, pos);
    }
}

void putstr_buf(char* buf, size_t& pos, const char* s) {
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        putc_buf(buf, pos, *s++);
    }
}

void putuint_buf(char* buf, size_t& pos, uint64_t val, int base, bool upper) {
    if (val == 0) {
        putc_buf(buf, pos, '0');
        return;
    }
    char tmp[20];
    int  i = 0;
    while (val > 0) {
        int d    = static_cast<int>(val % base);
        tmp[i++] = (d < 10) ? ('0' + d) : ((upper ? 'A' : 'a') + d - 10);
        val /= base;
    }
    while (i--) {
        putc_buf(buf, pos, tmp[i]);
    }
}

void putint_buf(char* buf, size_t& pos, int64_t val) {
    if (val < 0) {
        putc_buf(buf, pos, '-');
        val = -val;
    }
    putuint_buf(buf, pos, static_cast<uint64_t>(val), 10, false);
}

}  // anonymous namespace

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char   buf[PRINTF_BUF_SIZE];
    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            putc_buf(buf, pos, *fmt++);
            continue;
        }
        ++fmt;  // skip '%'

        // Parse length modifier
        enum class Len {
            None,
            L,
            LL
        };
        Len len = Len::None;
        if (*fmt == 'l') {
            ++fmt;
            if (*fmt == 'l') {
                ++fmt;
                len = Len::LL;
            } else {
                len = Len::L;
            }
        }

        char spec = *fmt;
        switch (spec) {
        case 'c': {
            char c = static_cast<char>(va_arg(ap, int));
            putc_buf(buf, pos, c);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            putstr_buf(buf, pos, s);
            break;
        }
        case 'd': {
            int64_t v;
            if (len == Len::LL) {
                v = va_arg(ap, int64_t);
            } else {
                v = static_cast<int64_t>(va_arg(ap, int));
            }
            putint_buf(buf, pos, v);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            int      base  = (spec == 'u') ? 10 : 16;
            bool     upper = (spec == 'X');
            if (len == Len::LL) {
                v = va_arg(ap, uint64_t);
            } else {
                v = static_cast<uint64_t>(va_arg(ap, unsigned int));
            }
            putuint_buf(buf, pos, v, base, upper);
            break;
        }
        case 'p': {
            auto v = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
            putstr_buf(buf, pos, "0x");
            putuint_buf(buf, pos, v, 16, false);
            break;
        }
        case '%':
            putc_buf(buf, pos, '%');
            break;
        default:
            putc_buf(buf, pos, '%');
            putc_buf(buf, pos, *fmt);
            break;
        }
        ++fmt;
    }

    flush_buf(buf, pos);
    va_end(ap);
    return 0;
}

}  // namespace cinux::user
