/**
 * @file kernel/lib/string.cpp
 * @brief Freestanding string/memory utility implementations
 *
 * Provides basic memory and string operations for the kernel.
 * All functions have C linkage to match the expected libc signatures.
 */

#include "kernel/lib/string.hpp"

extern "C" {

void* memset(void* dest, int val, size_t count) {
    auto* d = static_cast<uint8_t*>(dest);
    auto v = static_cast<uint8_t>(val);
    for (size_t i = 0; i < count; ++i) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count) {
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t count) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
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

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

}  // extern "C"
