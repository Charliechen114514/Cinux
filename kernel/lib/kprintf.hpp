/**
 * @file kernel/lib/kprintf.hpp
 * @brief Kernel formatted print functions (serial output)
 *
 * Provides printf-like formatting for kernel diagnostics.
 * Output goes to the serial port (COM1) and optionally to
 * the QEMU debug console (I/O port 0xE9).
 *
 * Supported format specifiers:
 *   %%  -- literal percent sign
 *   %c  -- character (int promoted to int via va_arg)
 *   %s  -- C string (nullptr prints as "(null)")
 *   %d  -- signed decimal (int)
 *   %u  -- unsigned decimal (unsigned int)
 *   %x  -- lowercase hexadecimal (uint64_t, no "0x" prefix)
 *   %X  -- uppercase hexadecimal (uint64_t, no "0x" prefix)
 *   %p  -- pointer (uint64_t, with "0x" prefix, 16 hex digits)
 *
 * Width modifiers:
 *   %Nd   -- minimum width N, right-align, space-padded
 *   %0Nd  -- minimum width N, right-align, zero-padded
 *   %-Nd  -- minimum width N, left-align, space-padded
 *   %-Ns  -- minimum width N, left-align, space-padded for strings
 *
 * @note %p always outputs a full 16-digit hex value with "0x" prefix,
 *       matching the checklist requirement.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

namespace cinux::lib {

/**
 * @brief Initialise the serial port used by kprintf
 *
 * Must be called once before any kprintf / kpanic / kvprintf call.
 * Configures COM1 to 115200 8N1 polling mode.
 */
void kprintf_init();

/**
 * @brief Variadic formatted print to serial (kernel equivalent of printf)
 *
 * @param fmt  printf-style format string
 * @param ...  variadic arguments matching the format specifiers
 */
void kprintf(const char* fmt, ...);

/**
 * @brief va_list variant of kprintf
 *
 * Useful when wrapping kprintf in another variadic function.
 *
 * @param fmt   printf-style format string
 * @param args  already-initialised va_list
 */
void kvprintf(const char* fmt, va_list args);

/**
 * @brief Kernel panic -- print message and halt
 *
 * Prints the formatted message to serial, then enters an
 * infinite cli;hlt loop.  This function never returns.
 *
 * @param fmt  printf-style format string
 * @param ...  variadic arguments
 */
[[noreturn]] void kpanic(const char* fmt, ...);

}  // namespace cinux::lib
