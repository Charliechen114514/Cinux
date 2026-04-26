/**
 * @file kernel/drivers/pit/pit.cpp
 * @brief PIT (Intel 8254) driver implementation
 *
 * Configures PIT channel 0 in square-wave mode, maintains a tick
 * counter, and provides uptime tracking.  The IRQ0 handler prints
 * a "[TICK] uptime: Ns" message once per second.
 */

#include "pit.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"

using cinux::arch::InterruptFrame;
using cinux::arch::PIC;
using cinux::io::io_outb;
using cinux::lib::kprintf;

namespace cinux::drivers {

// ============================================================
// Static storage
// ============================================================

lib::Atomic<uint64_t> PIT::tick_count_{0};
uint32_t              PIT::freq_hz_ = 100;

#ifdef CINUX_GUI
void (*PIT::tick_callback_)(void*) = nullptr;
void* PIT::tick_callback_ctx_      = nullptr;
#endif

// ============================================================
// PIT::init() -- configure channel 0 as square-wave generator
// ============================================================

void PIT::init(uint32_t freq_hz) {
    // Store the frequency for uptime calculations
    freq_hz_ = freq_hz;

    // Calculate the divisor: base_clock / desired_frequency
    // Clamp to 16-bit range [1, 65535]
    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;
    if (divisor > 65535) {
        divisor = 65535;
    }
    if (divisor == 0) {
        divisor = 1;
    }

    // Command byte 0x36:
    //   0x30 = channel 0, LSB-then-MSB access mode
    //   0x06 = square wave generator (mode 3)
    //   0x00 = binary counter (not BCD)
    // Total: 0x36
    io_outb(PitHW::COMMAND,
            PitHW::CMD_CHANNEL_0 | PitHW::CMD_LSB_MSB | PitHW::CMD_MODE_3 | PitHW::CMD_BINARY);

    // Write divisor: low byte first, then high byte
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>(divisor & 0xFF));
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    // Reset tick counter
    tick_count_ = 0;

    kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", freq_hz_, divisor);
}

// ============================================================
// PIT::irq0_handler() -- called from ISR stub on every tick
// ============================================================

void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    // Increment the global tick counter
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);

    // Signal End-Of-Interrupt to the PIC so the next IRQ can arrive
    PIC::send_eoi(0);

#ifdef CINUX_GUI
    invoke_tick_callback();
#endif

    cinux::proc::Scheduler::tick();
}

// ============================================================
// PIT::get_ticks() -- return the current tick count
// ============================================================

uint64_t PIT::get_ticks() {
    return tick_count_.load(lib::MemoryOrder::Relaxed);
}

// ============================================================
// PIT::get_uptime_ms() -- return uptime in milliseconds
// ============================================================

uint64_t PIT::get_uptime_ms() {
    // (tick_count * 1000) / freq_hz gives milliseconds
    return (tick_count_.load(lib::MemoryOrder::Relaxed) * 1000) / freq_hz_;
}

// ============================================================
// PIT::freq_hz() -- return configured frequency
// ============================================================

uint32_t PIT::freq_hz() {
    return freq_hz_;
}

// ============================================================
// PIT GUI tick callback (CINUX_GUI only)
// ============================================================

#ifdef CINUX_GUI

void PIT::set_tick_callback(void (*cb)(void*), void* ctx) {
    tick_callback_     = cb;
    tick_callback_ctx_ = ctx;
}

void PIT::invoke_tick_callback() {
    if (tick_callback_ != nullptr) {
        tick_callback_(tick_callback_ctx_);
    }
}

#endif

}  // namespace cinux::drivers

// ============================================================
// C-linkage bridge: called from irq0_stub in interrupts.S
// ============================================================

extern "C" void pit_irq0_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::PIT::irq0_handler(frame);
}
