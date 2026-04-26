/* ==============================================================
 * Cinux Mini Kernel - Format Function Implementation
 * ============================================================== */

#include "format.h"

#include <limits.h>

namespace cinux::mini::lib::detail {

// ============================================================
// Number Formatting Helpers
// ============================================================

int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1)
        return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == INT64_MIN) {
            const char* min_str = "-9223372036854775808";
            int         len     = 0;
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
        tmp[tmp_idx++] = '0' + (abs_val % 10);
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

int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1)
        return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char        tmp[20];
    int         tmp_idx = 0;

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

int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1)
        return 0;

    int  bit   = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found)
        bit = 0;

    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';

    return idx;
}

}  // namespace cinux::mini::lib::detail
