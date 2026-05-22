/**
 * @file kernel/proc/process_internal.hpp
 * @brief Shared internal state for process sub-modules
 *
 * Declares the TID counter and stack-virtual-address allocator shared
 * between task_builder.cpp and fork.cpp.  Not part of the public API.
 */

#pragma once

#include <stdint.h>

#include "kernel/lib/atomic.hpp"

namespace cinux::proc {

extern cinux::lib::Atomic<uint64_t> next_tid;

uint64_t alloc_stack_vaddr(uint64_t pages);

}  // namespace cinux::proc
