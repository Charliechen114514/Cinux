/**
 * @file kernel/mini/arch/x86_64/exception_handlers.cpp
 * @brief x86_64 Exception Handler Implementations
 *
 * Provides C handler functions for the #BP(3) and #PF(14) exceptions.
 * These functions are called by the ISR stubs in interrupts.S,
 * receiving an InterruptFrame* pointer as the argument, allowing
 * them to read/modify the register state at the time of the interrupt.
 *
 * Current implementation strategy: print exception info via serial and
 * continue execution (no crash/halt), fulfilling the milestone goal of
 * "trigger an exception without crashing, and display error information".
 */

#include "idt.hpp"
#include "lib/kprintf.h"

namespace {

using cinux::mini::arch::InterruptFrame;
using cinux::mini::lib::kprintf;

// ============================================================
// Helper: Print key registers from InterruptFrame
// ============================================================

/**
 * @brief Print a register snapshot from the interrupt stack frame
 *
 * @param frame Pointer to the interrupt stack frame
 * @param vec_name Exception name string (e.g. "#BP", "#PF")
 * @param vector Vector number
 */
void dump_interrupt_frame(const InterruptFrame* frame, const char* vec_name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", vec_name, vector);
    kprintf("  RIP   = 0x%016x   CS  = 0x%04x\n", frame->rip, frame->cs);
    kprintf("  RFLAGS= 0x%016x\n", frame->rflags);
    kprintf("  RSP   = 0x%016x   SS  = 0x%04x\n", frame->rsp, frame->ss);
    kprintf("  RAX=0x%016x  RBX=0x%016x\n", frame->rax, frame->rbx);
    kprintf("  RCX=0x%016x  RDX=0x%016x\n", frame->rcx, frame->rdx);
    kprintf("  RSI=0x%016x  RDI=0x%016x\n", frame->rsi, frame->rdi);
    kprintf("  RBP=0x%016x  R8 =0x%016x\n", frame->rbp, frame->r8);
    kprintf("  R9 =0x%016x  R10=0x%016x\n", frame->r9, frame->r10);
    kprintf("  R11=0x%016x  R12=0x%016x\n", frame->r11, frame->r12);
    kprintf("  R13=0x%016x  R14=0x%016x\n", frame->r13, frame->r14);
    kprintf("  R15=0x%016x\n", frame->r15);
    kprintf("  ERROR CODE = 0x%016x\n", frame->error_code);
    kprintf("========================================\n");
}

}  // anonymous namespace

// ============================================================
// Public Interface (extern "C", called from interrupts.S)
// ============================================================

/**
 * @brief #BP(3) Breakpoint exception handler
 *
 * Triggered when the INT3 instruction (opcode 0xCC) or
 * asm volatile("int $3") is executed. This is a trap exception;
 * when triggered, RIP points to the instruction after INT3,
 * so execution continues normally after IRETQ returns.
 *
 * @param frame Pointer to the interrupt stack frame, containing a
 *              complete register snapshot at the time of the exception
 */
extern "C" void handle_bp(InterruptFrame* frame) {
    // Step 1: Print breakpoint exception information
    dump_interrupt_frame(frame, "#BP", 3);

    // Step 2: Print a notice that this is a software breakpoint, safe to continue
    kprintf("[EXCEPTION] Breakpoint triggered at RIP=0x%x\n", frame->rip);
    kprintf("[EXCEPTION] This is a software breakpoint, continuing...\n");
}

/**
 * @brief #PF(14) Page fault exception handler
 *
 * Triggered when the CPU attempts to access a page table entry that
 * does not exist or has insufficient permissions. The CR2 register
 * holds the linear address that caused the fault.
 * Error code format:
 *   Bit 0 (P):   0=page not present, 1=protection violation
 *   Bit 1 (W/R): 0=read, 1=write
 *   Bit 2 (U/S): 0=kernel mode, 1=user mode
 *   Bit 3 (RSVD):1=reserved bits violation
 *   Bit 4 (I/D): 1=instruction fetch (code page fault)
 *
 * @param frame Pointer to the interrupt stack frame; frame->error_code
 *              contains the page fault error code
 *
 * @note The current implementation only prints information and continues,
 *       without performing any page repair. Once VMM is added in the future,
 *       this will become the core of the page fault handler.
 */
extern "C" void handle_pf(InterruptFrame* frame) {
    // Step 1: Read CR2 to get the faulting address
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    // Step 2: Parse the error code
    uint64_t    err      = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    // Step 3: Print detailed page fault information
    dump_interrupt_frame(frame, "#PF", 14);
    kprintf("[EXCEPTION] Page Fault: %s %s %s%s%s\n", present, access, mode, reserved, fetch);
    kprintf("[EXCEPTION] Faulting address (CR2) = 0x%016x\n", fault_addr);
    kprintf("[EXCEPTION] Continuing execution...\n");
}
