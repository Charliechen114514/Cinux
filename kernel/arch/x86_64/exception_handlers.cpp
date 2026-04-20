/**
 * @file kernel/arch/x86_64/exception_handlers.cpp
 * @brief CPU exception handler implementations for the big kernel
 *
 * Provides C handler functions for all configured CPU exceptions (vectors 0-14).
 * Called by ISR stubs in interrupts.S with an InterruptFrame* argument.
 *
 * Policy:
 *   - Non-fatal exceptions (#BP, #DB): print info and continue via IRETQ
 *   - Fatal exceptions (all others): print register dump, then cli;hlt forever
 */

#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

#include <stdint.h>

namespace {

using cinux::arch::InterruptFrame;
using cinux::lib::kprintf;
using cinux::mm::g_vmm;

void dump_registers(const InterruptFrame* frame,
                    const char* name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", name, vector);
    kprintf("  RIP   = %p   CS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rip),
            static_cast<unsigned>(frame->cs));
    kprintf("  RFLAGS= %p\n",
            reinterpret_cast<void*>(frame->rflags));
    kprintf("  RSP   = %p   SS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rsp),
            static_cast<unsigned>(frame->ss));
    kprintf("  RAX=%p  RBX=%p\n",
            reinterpret_cast<void*>(frame->rax),
            reinterpret_cast<void*>(frame->rbx));
    kprintf("  RCX=%p  RDX=%p\n",
            reinterpret_cast<void*>(frame->rcx),
            reinterpret_cast<void*>(frame->rdx));
    kprintf("  RSI=%p  RDI=%p\n",
            reinterpret_cast<void*>(frame->rsi),
            reinterpret_cast<void*>(frame->rdi));
    kprintf("  RBP=%p  R8 =%p\n",
            reinterpret_cast<void*>(frame->rbp),
            reinterpret_cast<void*>(frame->r8));
    kprintf("  R9 =%p  R10=%p\n",
            reinterpret_cast<void*>(frame->r9),
            reinterpret_cast<void*>(frame->r10));
    kprintf("  R11=%p  R12=%p\n",
            reinterpret_cast<void*>(frame->r11),
            reinterpret_cast<void*>(frame->r12));
    kprintf("  R13=%p  R14=%p\n",
            reinterpret_cast<void*>(frame->r13),
            reinterpret_cast<void*>(frame->r14));
    kprintf("  R15=%p\n",
            reinterpret_cast<void*>(frame->r15));
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}

[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // anonymous namespace

// ============================================================
// Exception handlers (C-linkage, called from ISR stubs)
// ============================================================

extern "C" {

void handle_db(InterruptFrame* frame) {
    dump_registers(frame, "#DB", 1);
    kprintf("[EXCEPTION] Debug exception, continuing...\n");
}

void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}

void handle_de(InterruptFrame* frame) {
    dump_registers(frame, "#DE", 0);
    kprintf("[FATAL] Divide Error -- halting.\n");
    fatal_halt();
}

void handle_nmi(InterruptFrame* frame) {
    dump_registers(frame, "NMI", 2);
    kprintf("[FATAL] Non-maskable Interrupt -- halting.\n");
    fatal_halt();
}

void handle_of(InterruptFrame* frame) {
    dump_registers(frame, "#OF", 4);
    kprintf("[FATAL] Overflow -- halting.\n");
    fatal_halt();
}

void handle_br(InterruptFrame* frame) {
    dump_registers(frame, "#BR", 5);
    kprintf("[FATAL] BOUND Range Exceeded -- halting.\n");
    fatal_halt();
}

void handle_ud(InterruptFrame* frame) {
    dump_registers(frame, "#UD", 6);
    kprintf("[FATAL] Invalid Opcode -- halting.\n");
    fatal_halt();
}

void handle_nm(InterruptFrame* frame) {
    dump_registers(frame, "#NM", 7);
    kprintf("[FATAL] Device Not Available -- halting.\n");
    fatal_halt();
}

void handle_df(InterruptFrame* frame) {
    dump_registers(frame, "#DF", 8);
    kprintf("[FATAL] Double Fault (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_ts(InterruptFrame* frame) {
    dump_registers(frame, "#TS", 10);
    kprintf("[FATAL] Invalid TSS (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_np(InterruptFrame* frame) {
    dump_registers(frame, "#NP", 11);
    kprintf("[FATAL] Segment Not Present (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_ss(InterruptFrame* frame) {
    dump_registers(frame, "#SS", 12);
    kprintf("[FATAL] Stack Fault (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);
    kprintf("[FATAL] General Protection Fault (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;

    // Demand-paging: try to allocate a page for not-present faults
    if ((err & 0x01) == 0) {
        uint64_t virt_page = fault_addr & ~0xFFFULL;
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        if (phys != 0 && g_vmm.map(virt_page, phys,
                                    cinux::arch::FLAG_PRESENT |
                                    cinux::arch::FLAG_WRITABLE)) {
            kprintf("[VMM] Demand-paged %p -> phys %p\n",
                    reinterpret_cast<void*>(virt_page),
                    reinterpret_cast<void*>(phys));
            return;
        }
    }

    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_registers(frame, "#PF", 14);
    kprintf("[FATAL] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[FATAL] Faulting address (CR2) = %p -- halting.\n",
            reinterpret_cast<void*>(fault_addr));
    fatal_halt();
}

}  // extern "C"
