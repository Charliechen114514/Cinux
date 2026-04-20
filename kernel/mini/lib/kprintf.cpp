/* ==============================================================
 * Cinux Mini Kernel - Kernel Print Function Implementation
 * ============================================================== */

#include "kprintf.h"
#include "private/vkprintf_impl.h"

#include <stdarg.h>
#include <stdint.h>

#include "driver/serial.h"


namespace {

using namespace cinux::mini::lib::detail;

// ============================================================
// Debug Console Output Helper
// ============================================================

void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

}  // namespace


namespace cinux::mini::lib {

// ============================================================
// kprintf - Serial Output
// ============================================================
void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    auto& serial = serial::get_initial_serial();
    vkprintf_impl([&](char c) { serial.putc(c); }, format, args);

    va_end(args);
}

// ============================================================
// kdebugf - Debug Console Output
// ============================================================
void kdebugf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    vkprintf_impl([](char c) { debugcon_putc(c); }, format, args);

    va_end(args);
}

}  // namespace cinux::mini::lib
