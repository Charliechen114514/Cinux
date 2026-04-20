/**
 * @file kernel/lib/kprintf.cpp
 * @brief Kernel formatted print implementation (serial output)
 *
 * Internal structure:
 *   1. The hardware-independent formatting engine lives in
 *      kernel/lib/private/vkprintf_impl.hpp (shared with host tests).
 *   2. Public kprintf / kvprintf / kpanic wrappers that feed characters
 *      to the global serial port instance.
 */

#include "kernel/lib/kprintf.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/drivers/serial.hpp"
#include "kernel/lib/private/vkprintf_impl.hpp"

namespace {

using cinux::drivers::Serial;
using cinux::drivers::SERIAL_COM1;
using cinux::lib::detail::vkprintf_impl;

// ============================================================
// Global serial instance (singleton for big kernel)
// ============================================================

static Serial g_serial(SERIAL_COM1);

}  // anonymous namespace

namespace cinux::lib {

// ============================================================
// kprintf_init -- one-time serial port setup for kprintf
// ============================================================

void kprintf_init() {
    g_serial.init();
}

// ============================================================
// kprintf -- variadic serial print
// ============================================================

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}

// ============================================================
// kvprintf -- va_list serial print
// ============================================================

void kvprintf(const char* fmt, va_list args) {
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
}

// ============================================================
// kpanic -- print + halt
// ============================================================

void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);

    // Halt forever
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // namespace cinux::lib
