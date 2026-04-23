/**
 * @file test/unit/host_spinlock.cpp
 * @brief Host-side stub for cinux::proc::Spinlock
 *
 * Provides the linkable symbols for Spinlock::acquire(), release(),
 * IrqGuard ctor/dtor, and InterruptGuard ctor/dtor so that kernel
 * code using <kernel/proc/sync.hpp> can compile and run on the host.
 *
 * Uses std::atomic instead of x86 inline asm.
 */

#include "kernel/proc/sync.hpp"

namespace cinux::proc {

void Spinlock::acquire() {
    while (__atomic_exchange_n(&locked_, true, __ATOMIC_ACQUIRE)) {
    }
}

void Spinlock::release() {
    __atomic_store_n(&locked_, false, __ATOMIC_RELEASE);
}

Spinlock::IrqGuard::IrqGuard(Spinlock* lock) : lock_(lock), saved_flags_(0) {
    lock_->acquire();
}

Spinlock::IrqGuard::~IrqGuard() {
    lock_->release();
}

InterruptGuard::InterruptGuard() : saved_flags_(0) {}
InterruptGuard::~InterruptGuard() = default;

}  // namespace cinux::proc
