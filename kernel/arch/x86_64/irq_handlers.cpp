/**
 * @file kernel/arch/x86_64/irq_handlers.cpp
 * @brief Hardware IRQ handler implementations and IDT registration
 *
 * Provides C handler functions for all 16 hardware IRQ lines (IRQ0-15)
 * after PIC remapping.  Also provides irq_init() which registers all
 * IRQ ISR stubs into the IDT.
 *
 * Handler policy:
 *   - IRQ0 (PIT): forwards to PIT::irq0_handler() which tracks ticks
 *     and sends EOI internally.
 *   - IRQ1-15 (default): ack the interrupt via PIC EOI and ignore.
 *     This prevents unhandled IRQs from causing a Double Fault.
 *
 * Dependencies:
 *   - PIC must be initialised before any IRQ arrives
 *   - IDT must exist (irq_init() writes to it)
 */

#include <stdint.h>

#include "gdt.hpp"
#include "idt.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/lib/kprintf.hpp"
#include "pic.hpp"

using cinux::arch::ExceptionVector;
using cinux::arch::GDT_KERNEL_CODE;
using cinux::arch::IDT;
using cinux::arch::IDTGateType;
using cinux::arch::IDTPrivilege;
using cinux::arch::InterruptFrame;
using cinux::arch::PIC;
using cinux::arch::g_idt;
using cinux::arch::make_idt_attr;
using cinux::lib::kprintf;

// ============================================================
// ISR stubs (defined in interrupts.S)
// ============================================================

extern "C" {
void irq0_stub();
void irq1_stub();
void irq2_stub();
void irq3_stub();
void irq4_stub();
void irq5_stub();
void irq6_stub();
void irq7_stub();
void irq8_stub();
void irq9_stub();
void irq10_stub();
void irq11_stub();
void irq12_stub();
void irq13_stub();
void irq14_stub();
void irq15_stub();
}  // extern "C"

// ============================================================
// IRQ routing table (constexpr, data-driven)
// ============================================================

struct IRQRoute {
    uint8_t   vector;
    IDT::Stub stub;
};

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  {0x22, irq2_stub},  {0x23, irq3_stub},
    {0x24, irq4_stub},  {0x25, irq5_stub},  {0x26, irq6_stub},  {0x27, irq7_stub},
    {0x28, irq8_stub},  {0x29, irq9_stub},  {0x2A, irq10_stub}, {0x2B, irq11_stub},
    {0x2C, irq12_stub}, {0x2D, irq13_stub}, {0x2E, irq14_stub}, {0x2F, irq15_stub},
};

static constexpr uint8_t kIRQAttr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);

// ============================================================
// Default IRQ handler (IRQ1-15)
// ============================================================

extern "C" {

/**
 * @brief Default handler for unconfigured IRQ lines
 *
 * Simply sends EOI to the master PIC so the interrupt does not
 * stall further IRQ delivery.  Since we do not know which specific
 * IRQ fired from this shared handler, we send master-only EOI.
 *
 * @param frame  Interrupt stack frame (unused)
 */
void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);
}

#ifndef CINUX_GUI
/**
 * @brief Default IRQ12 handler when GUI mode is disabled
 *
 * In non-GUI builds the mouse driver is not compiled, so we provide
 * a stub that simply acknowledges the interrupt.  In GUI builds the
 * real implementation lives in mouse.cpp and overrides this weak alias.
 *
 * @param frame  Interrupt stack frame (unused)
 */
void mouse_irq12_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(12);
}
#endif

}  // extern "C"

// ============================================================
// irq_init() -- register all IRQ stubs into the IDT
// ============================================================

/**
 * @brief Register all hardware IRQ handlers into the IDT
 *
 * Installs ISR stubs for INT vectors 0x20-0x2F (IRQ0-15 after
 * PIC remapping).  All use kernel interrupt gates (DPL=0, IF cleared).
 *
 * Must be called after idt_init() and pic_init().
 */
extern "C" void irq_init() {
    kprintf("[IRQ] Registering IRQ handlers (0x20-0x2F)...\n");

    for (const auto& route : k_irq_routes) {
        g_idt.set_handler(static_cast<ExceptionVector>(route.vector), route.stub, GDT_KERNEL_CODE,
                          kIRQAttr, 0);
    }

    kprintf("[IRQ] All IRQ handlers registered.\n");
}
