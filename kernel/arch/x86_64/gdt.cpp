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

    entries_[1] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // Set up IST1 to point at the top of the dedicated Double Fault stack
    tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);

    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[6] = tss_high_entry(tss_addr);

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
