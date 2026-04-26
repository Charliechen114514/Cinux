/**
 * @file kernel/lib/kprintf.hpp
 * @brief Kernel formatted print functions (multi-backend sink architecture)
 *
 * Provides printf-like formatting for kernel diagnostics.
 * Supports multiple output backends (sinks) that can be registered at
 * runtime -- e.g. serial port, framebuffer console, QEMU debug console.
 *
 * Sink registration:
 *   using OutputSink = void(*)(char, void* ctx);
 *   kprintf_register_sink(OutputSink fn, void* ctx);
 *
 * Up to 8 sinks may be active simultaneously.  kprintf/kvprintf/kpanic
 * iterate all enabled sinks and invoke each with every formatted character.
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

// ============================================================
// Sink type and registration
// ============================================================

/// Callback type for kprintf output backends.
/// @param c    Character to output
/// @param ctx  Opaque context pointer supplied at registration time
using OutputSink = void (*)(char c, void* ctx);

/// Maximum number of concurrently registered sinks
static constexpr uint32_t KPRINTF_MAX_SINKS = 8;

/**
 * @brief Register an output sink for kprintf
 *
 * @param fn   Sink callback function (must not be null)
 * @param ctx  Opaque context passed to fn on each character
 */
void kprintf_register_sink(OutputSink fn, void* ctx);

// ============================================================
// Initialization and formatted output
// ============================================================

/**
 * @brief Initialise kprintf with the default serial sink
 *
 * Must be called once before any kprintf / kpanic / kvprintf call.
 * Configures COM1 to 115200 8N1 polling mode and registers it as
 * the first output sink.
 */
void kprintf_init();

/**
 * @brief Variadic formatted print to all registered sinks
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
 * Prints the formatted message to all sinks, then enters an
 * infinite cli;hlt loop.  This function never returns.
 *
 * @param fmt  printf-style format string
 * @param ...  variadic arguments
 */
[[noreturn]] void kpanic(const char* fmt, ...);

}  // namespace cinux::lib
