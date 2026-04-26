#pragma once

#include <stdint.h>

namespace cinux::proc {

struct Task;

struct PerCPU {
    Task*    current;
    uint64_t kernel_stack;
    uint64_t gs_page_vaddr;

    void update_syscall_stack(uint64_t stack_top) {
        kernel_stack = stack_top;
        if (gs_page_vaddr != 0) {
            *reinterpret_cast<volatile uint64_t*>(gs_page_vaddr) = stack_top;
        }
    }
};

extern PerCPU g_per_cpu;

}  // namespace cinux::proc
