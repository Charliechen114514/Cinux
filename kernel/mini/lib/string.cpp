/**
 * @file kernel/mini/lib/string.cpp
 * @brief Freestanding memory utility function implementations
 */

#include "string.h"

void* memset(void* dest, int val, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    uint8_t  v = static_cast<uint8_t>(val);
    for (size_t i = 0; i < count; i++) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count) {
    uint8_t*       d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    uint8_t*       d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}
