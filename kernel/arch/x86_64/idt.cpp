/**
 * @file kernel/arch/x86_64/idt.cpp
 * @brief IDT initialization and loading for the big kernel
 *
 * Configures IDT entries using a data-driven routing table, then loads
 * the IDT via LIDT. ISR stubs are in interrupts.S; C handlers are in
 * exception_handlers.cpp.
 */

#include "kernel/arch/x86_64/idt.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"

namespace cinux::arch {

IDT g_idt;

// ============================================================
// ISR stubs (defined in interrupts.S)
// ============================================================

extern "C" {
void isr_de_stub();
void isr_db_stub();
void isr_nmi_stub();
void isr_bp_stub();
void isr_of_stub();
void isr_br_stub();
void isr_ud_stub();
void isr_nm_stub();
void isr_df_stub();
void isr_ts_stub();
void isr_np_stub();
void isr_ss_stub();
void isr_gp_stub();
void isr_pf_stub();
}  // extern "C"

// ============================================================
// Exception handlers (defined in exception_handlers.cpp)
// ============================================================

extern "C" {
void handle_de(InterruptFrame*);
void handle_db(InterruptFrame*);
void handle_nmi(InterruptFrame*);
void handle_bp(InterruptFrame*);
void handle_of(InterruptFrame*);
void handle_br(InterruptFrame*);
void handle_ud(InterruptFrame*);
void handle_nm(InterruptFrame*);
void handle_df(InterruptFrame*);
void handle_ts(InterruptFrame*);
void handle_np(InterruptFrame*);
void handle_ss(InterruptFrame*);
void handle_gp(InterruptFrame*);
void handle_pf(InterruptFrame*);
}  // extern "C"

// ============================================================
// IDT Implementation
// ============================================================

void IDT::set_handler(ExceptionVector vector, Stub stub, uint16_t selector, uint8_t type_attr,
                      uint8_t ist) {
    const auto vec  = static_cast<uint8_t>(vector);
    const auto addr = reinterpret_cast<uint64_t>(stub);

    entries_[vec].offset_low  = static_cast<uint16_t>(addr & 0xFFFF);
    entries_[vec].offset_mid  = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    entries_[vec].offset_high = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    entries_[vec].selector    = selector;
    entries_[vec].ist         = ist;
    entries_[vec].type_attr   = type_attr;
    entries_[vec].reserved    = 0;
}

void IDT::init() {
    // Clear all entries
    for (auto& entry : entries_) {
        entry = Entry{};
    }

    // Data-driven exception routing: {vector, stub, privilege, gate_type, ist}
    // IST 1 is used for #DF (Double Fault) to get a dedicated stack.
    struct Route {
        ExceptionVector vector;
        Stub            stub;
        IDTPrivilege    priv;
        IDTGateType     gate;
        uint8_t         ist;
    };

    const Route routes[] = {
        {ExceptionVector::DE, isr_de_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::DB, isr_db_stub, IDTPrivilege::Kernel, IDTGateType::Trap, 0},
        {ExceptionVector::NMI, isr_nmi_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::BP, isr_bp_stub, IDTPrivilege::User, IDTGateType::Trap, 0},
        {ExceptionVector::OF, isr_of_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::BR, isr_br_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::UD, isr_ud_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::NM, isr_nm_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::DF, isr_df_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 1},
        {ExceptionVector::TS, isr_ts_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::NP, isr_np_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::SS, isr_ss_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::GP, isr_gp_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
        {ExceptionVector::PF, isr_pf_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 0},
    };

    for (const auto& r : routes) {
        set_handler(r.vector, r.stub, GDT_KERNEL_CODE, make_idt_attr(r.priv, r.gate), r.ist);
    }

    // Load IDTR
    idtr_.limit = static_cast<uint16_t>(sizeof(entries_) - 1);
    idtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}

void IDT::load() {
    __asm__ volatile("lidt %[idtr]\n\t" : : [idtr] "m"(idtr_) : "memory");
}

}  // namespace cinux::arch
