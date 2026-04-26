/**
 * @file kernel/proc/pid.hpp
 * @brief PID allocator for process identification
 *
 * Manages a bounded pool of process identifiers (PIDs) from 1 to PID_MAX.
 * PIDs are allocated sequentially and recycled when freed.  The allocator
 * is designed as a modern C++ class with RAII semantics -- no free
 * functions wrapping static globals.
 *
 * PID 0 is reserved (unused / idle process).
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::proc {

class PidAllocator {
public:
    static constexpr int PID_NONE = 0;
    static constexpr int PID_MAX  = 256;

    PidAllocator();

    /**
     * @brief Allocate a fresh PID
     *
     * Returns the lowest available PID starting from 1.  If all PIDs
     * are in use, returns PID_NONE (0).
     *
     * @return Allocated PID, or PID_NONE if exhausted
     */
    int alloc();

    /**
     * @brief Release a previously allocated PID
     *
     * Marks the PID as available for future alloc() calls.
     * Freeing PID_NONE or an already-free PID is safe (no-op).
     *
     * @param pid  The PID to release
     */
    void free(int pid);

    /**
     * @brief Check whether a PID is currently allocated
     *
     * @param pid  The PID to query
     * @return true if the PID is in use, false otherwise
     */
    bool is_allocated(int pid) const;

    /**
     * @brief Get the number of currently allocated PIDs
     *
     * @return Count of in-use PIDs
     */
    int count() const;

private:
    bool in_use_[PID_MAX + 1];
    int  next_hint_;
};

/// Global PID allocator instance.
extern PidAllocator g_pid_alloc;

}  // namespace cinux::proc
