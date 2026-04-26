/**
 * @file user/libc/string.cpp
 * @brief Freestanding string / memory utility implementations
 */

#include "libc/string.hpp"

#include <stdint.h>

namespace cinux::user {

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return static_cast<int>(*a) - static_cast<int>(*b);
        }
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

void* memset(void* dest, int c, size_t n) {
    auto*         d = static_cast<uint8_t*>(dest);
    const uint8_t v = static_cast<uint8_t>(c);
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    auto*       d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

}  // namespace cinux::user
