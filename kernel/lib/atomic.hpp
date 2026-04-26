#pragma once

#include <stdint.h>

namespace cinux::lib {

enum class MemoryOrder {
    Relaxed = __ATOMIC_RELAXED,
    Acquire = __ATOMIC_ACQUIRE,
    Release = __ATOMIC_RELEASE,
    AcqRel  = __ATOMIC_ACQ_REL,
    SeqCst  = __ATOMIC_SEQ_CST,
};

template <typename T>
class Atomic {
    static_assert(__is_trivially_copyable(T), "Atomic requires a trivially copyable type");
    alignas(T) T value_;

public:
    constexpr Atomic() = default;
    constexpr explicit Atomic(T v) : value_(v) {}

    Atomic(const Atomic&)            = delete;
    Atomic& operator=(const Atomic&) = delete;

    T load(MemoryOrder order = MemoryOrder::SeqCst) const {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    void store(T v, MemoryOrder order = MemoryOrder::SeqCst) {
        __atomic_store_n(&value_, v, static_cast<int>(order));
    }

    T fetch_add(T delta, MemoryOrder order = MemoryOrder::SeqCst) {
        return __atomic_fetch_add(&value_, delta, static_cast<int>(order));
    }

    T exchange(T desired, MemoryOrder order = MemoryOrder::SeqCst) {
        return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
    }

    bool compare_exchange(T& expected, T desired, MemoryOrder order = MemoryOrder::SeqCst) {
        return __atomic_compare_exchange_n(&value_, &expected, desired, false,
                                           static_cast<int>(order), static_cast<int>(order));
    }

    T operator=(T v) {
        store(v);
        return v;
    }

    operator T() const { return load(); }
};

}  // namespace cinux::lib
