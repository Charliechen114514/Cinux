/* ==============================================================
 * Cinux Mini Kernel - Format Function Declarations
 * ============================================================== */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::mini::lib::detail {

// ============================================================
// Number Formatting Functions
// ============================================================

/**
 * @brief Format a signed 64-bit integer as decimal string
 * @param value The value to format
 * @param buffer Output buffer
 * @param buffer_size Size of the output buffer
 * @return Number of characters written (excluding null terminator)
 */
int format_decimal(int64_t value, char* buffer, int buffer_size);

/**
 * @brief Format an unsigned 64-bit integer as hexadecimal string
 * @param value The value to format
 * @param buffer Output buffer
 * @param buffer_size Size of the output buffer
 * @param lowercase Use lowercase letters (a-f) if true, uppercase (A-F) if false
 * @return Number of characters written (excluding null terminator)
 */
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase);

/**
 * @brief Format an unsigned 64-bit integer as binary string
 * @param value The value to format
 * @param buffer Output buffer
 * @param buffer_size Size of the output buffer
 * @return Number of characters written (excluding null terminator)
 */
int format_binary(uint64_t value, char* buffer, int buffer_size);

}  // namespace cinux::mini::lib::detail
