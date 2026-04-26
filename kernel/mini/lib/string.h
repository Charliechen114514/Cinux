/**
 * @file kernel/mini/lib/string.h
 * @brief Freestanding memory utility functions
 *
 * Provides basic memset, memcpy, and memmove for the mini kernel.
 * The freestanding environment has no standard library, so these
 * must be implemented manually.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Fill memory region with a byte value
void* memset(void* dest, int val, size_t count);

/// Copy memory region (source and dest must not overlap)
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);

/// Copy memory region (source and dest may overlap)
void* memmove(void* dest, const void* src, size_t count);

#ifdef __cplusplus
}
#endif
