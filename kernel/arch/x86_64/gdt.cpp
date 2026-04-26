/**
 * @file kernel/arch/x86_64/gdt.cpp
 * @brief GDT initialization and loading for the big kernel
 *
 * Fills all GDT entries (null / kernel code / kernel data / user code /
 * user data / TSS), loads via LGDT, flushes segment registers, and loads TR.
 */

#include "kernel/arch/x86_64/gdt.hpp"

#include <stdint.h>

namespace cinux::arch {

GDT g_gdt;

void GDT::init() {
    entries_[0] = null_entry();

    // Idx 1 (0x08): unused (TLS placeholder)
    entries_[1] = null_entry();

    // Idx 2 (0x10): Kernel Code — SYSCALL CS
    entries_[2] =
        segment_entry(SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData |
                          SegmentAccess::Executable | SegmentAccess::ReadWrite,
                      SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // Idx 3 (0x18): Kernel Data — SYSCALL SS
    entries_[3] = segment_entry(SegmentAccess::Present | SegmentAccess::Ring0 |
                                    SegmentAccess::CodeData | SegmentAccess::ReadWrite,
                                SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 4 (0x20): User32 Code — STAR[63:48] base for SYSRETQ
    entries_[4] =
        segment_entry(SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                          SegmentAccess::Executable | SegmentAccess::ReadWrite,
                      SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 5 (0x28): User Data — SYSRETQ SS = 0x28|3 = 0x2B
    entries_[5] = segment_entry(SegmentAccess::Present | SegmentAccess::Ring3 |
                                    SegmentAccess::CodeData | SegmentAccess::ReadWrite,
                                SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Idx 6 (0x30): User64 Code — SYSRETQ CS = 0x30|3 = 0x33
    entries_[6] =
        segment_entry(SegmentAccess::Present | SegmentAccess::Ring3 | SegmentAccess::CodeData |
                          SegmentAccess::Executable | SegmentAccess::ReadWrite,
                      SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // Set up IST1 to point at the top of the dedicated Double Fault stack
    tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);

    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[7]         = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[8]         = tss_high_entry(tss_addr);

    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}

void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;
}

void GDT::load() {
    __asm__ volatile(
        "lgdt %[gdtr]\n\t"
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(gdtr_), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    const uint16_t tss_sel = GDT_TSS;
    __asm__ volatile("ltr %[sel]\n\t" : : [sel] "r"(tss_sel) : "memory");
}

}  // namespace cinux::arch
