/**
 * @file kernel/proc/pid.cpp
 * @brief PID allocator implementation
 */

#include "proc/pid.hpp"

namespace cinux::proc {

// ============================================================
// Global instance
// ============================================================

PidAllocator g_pid_alloc;

// ============================================================
// Construction
// ============================================================

PidAllocator::PidAllocator() : next_hint_(1) {
    for (int i = 0; i <= PID_MAX; ++i) {
        in_use_[i] = false;
    }
}

int PidAllocator::alloc() {
    // Scan from next_hint_ to PID_MAX, wrapping around to 1
    for (int i = 0; i < PID_MAX; ++i) {
        int candidate = next_hint_ + i;
        if (candidate > PID_MAX) {
            candidate -= PID_MAX;
        }
        if (candidate == 0) {
            candidate = 1;
        }
        if (!in_use_[candidate]) {
            in_use_[candidate] = true;
            // Advance hint past this PID for next allocation
            next_hint_         = (candidate >= PID_MAX) ? 1 : candidate + 1;
            return candidate;
        }
    }
    return PID_NONE;
}

void PidAllocator::free(int pid) {
    if (pid <= 0 || pid > PID_MAX) {
        return;
    }
    if (!in_use_[pid]) {
        return;
    }
    in_use_[pid] = false;

    // Pull hint back if we freed a lower PID
    if (pid < next_hint_) {
        next_hint_ = pid;
    }
}

bool PidAllocator::is_allocated(int pid) const {
    if (pid <= 0 || pid > PID_MAX) {
        return false;
    }
    return in_use_[pid];
}

int PidAllocator::count() const {
    int n = 0;
    for (int i = 1; i <= PID_MAX; ++i) {
        if (in_use_[i]) {
            ++n;
        }
    }
    return n;
}

}  // namespace cinux::proc
