# COW 页错误处理与 context_switch 扩展

## 概览

本篇讲解 tag 035 中两个关键的底层修改：COW 页错误处理器 handle_cow_fault() 的实现，以及 context_switch.S 的 GS MSR 保存/恢复扩展。这两个改动让 fork 的 CoW 策略真正生效——子进程写入共享页时能被正确拦截并处理，同时保证多任务场景下 syscall 的 GS 寄存器状态不会混乱。

关键设计决策：#PF handler 使用无锁分配路径避免递归死锁；GS MSR 通过 rdmsr/wrmsr 在 context_switch 中显式保存/恢复。

## 架构图

```
Page Fault (write to CoW page):
  CPU push error code (P=1, W/R=1, U/S=1)
    → ISR stub → handle_pf()
      → stack guard detection (scheduler task + boot stack)
      → demand-paging? (P=0) → alloc_page_locked + map_nolock
      → CoW fault? (P=1,W=1,U=1) → handle_cow_fault()
        → get_pte() → 4-level walk (PML4→PDPT→PD→PT)
        → 4 checks: present, !writable, FLAG_COW
        → alloc new page → memcpy via KERNEL_VMA → update PTE → flush TLB
      → fatal error (dump registers + kpanic)

context_switch (with GS MSR):
  save: r15-rbx, rsp, rip, rdmsr(GS_BASE 0xC0000101), rdmsr(KERNEL_GS_BASE 0xC0000102)
  restore: wrmsr(GS_BASE), wrmsr(KERNEL_GS_BASE), rsp, rbx-r15, jmp rip

fork() GS init:
  child->ctx.gs_base = 0                    // user default
  child->ctx.kgs_base = g_per_cpu.gs_page_vaddr  // per-CPU data page

TaskBuilder::build() GS init:
  task->ctx.gs_base = 0
  task->ctx.kgs_base = g_per_cpu.gs_page_vaddr
```

## 代码精讲

### handle_cow_fault() — PTE 查找与验证

```cpp
bool handle_cow_fault(uint64_t fault_vaddr) {
    auto* task = Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) return false;

    uint64_t   pml4_phys = task->addr_space->pml4_phys();
    PageEntry* pte       = get_pte(pml4_phys, fault_vaddr);
    if (pte == nullptr) return false;

    if (!pte->is_present())  return false;
    if (pte->raw & FLAG_WRITABLE) return false;
    if (!(pte->raw & FLAG_COW))   return false;
```

四重检查确保只处理真正的 CoW fault：task 必须存在且有地址空间，PTE 必须存在（walk 成功），页必须 present（不是 demand-paging），页必须不可写（已经被 fork 标记为只读），必须有 FLAG_COW 标记（区分 CoW 和普通的权限违反）。任何一关不通过就返回 false，让调用者继续尝试其他处理路径。

注意 handle_cow_fault 使用普通的 g_pmm.alloc_page() 而不是 alloc_page_locked——因为 handle_cow_fault 在 IF=1 的上下文中被调用（从 handle_pf 返回后走正常的 CoW 路径），不在 Interrupt gate 下运行。这和 demand-paging 的 alloc_page_locked 不同。

### get_pte() — 四级页表 walk

```cpp
PageEntry* get_pte(uint64_t pml4_phys, uint64_t virt) {
    auto*      pml4  = phys_to_virt(pml4_phys);
    PageEntry& pml4e = pml4[PML4_INDEX(virt)];
    if (!pml4e.is_present()) return nullptr;

    auto*      pdpt  = phys_to_virt(pml4e.phys_addr());
    PageEntry& pdpte = pdpt[PDPT_INDEX(virt)];
    if (!pdpte.is_present()) return nullptr;

    auto*      pd  = phys_to_virt(pdpte.phys_addr());
    PageEntry& pde = pd[PD_INDEX(virt)];
    if (!pde.is_present()) return nullptr;

    auto* pt = phys_to_virt(pde.phys_addr());
    return &pt[PT_INDEX(virt)];
}
```

从 PML4 到 PT 逐级 walk，任何一级不存在就返回 nullptr。所有物理地址都通过 phys_to_virt (phys + KERNEL_VMA) 转换为可访问的虚拟地址。索引提取使用 paging_config.hpp 中的移位和掩码宏（PML4_INDEX、PDPT_INDEX、PD_INDEX、PT_INDEX）。get_pte 是一个 anonymous namespace 中的静态辅助函数，仅供 handle_cow_fault 和 fork 使用。

### handle_cow_fault() — 页复制与 PTE 更新

```cpp
    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();

    auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < cinux::arch::PAGE_SIZE; i++) {
        dst[i] = src[i];
    }

    pte->set_phys_addr(new_phys);
    pte->raw |= FLAG_WRITABLE;
    pte->raw &= ~FLAG_COW;

    cinux::arch::flush_tlb(fault_vaddr & ~(cinux::arch::PAGE_SIZE - 1));

    return true;
```

四个步骤：分配新页、复制内容（逐字节通过高半区映射）、更新 PTE（新物理地址 + 可写 + 清 CoW）、刷新 TLB。flush_tlb 执行 invlpg 指令，参数是页对齐的虚拟地址——确保 CPU 不再缓存旧的只读映射。

### #PF handler 集成

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
    uint64_t err = frame->error_code;

    // Stack guard page detection ...
    // 1. Scheduler task guard: cur->kernel_stack_guard_page
    // 2. Boot stack guard: __boot_guard_start/end

    // Demand-paging (P=0): lock-free allocation
    if ((err & 0x01) == 0) {
        uint64_t map_flags = FLAG_PRESENT | FLAG_WRITABLE;
        if (is_user_vaddr(fault_addr)) map_flags |= FLAG_USER;
        uint64_t phys = g_pmm.alloc_page_locked();
        if (phys != 0) {
            bool ok = g_vmm.map_nolock(virt_page, phys, map_flags, &cur_cr3);
            if (ok) {
                kprintf("[VMM] Demand-paged %p -> phys %p\n", ...);
                return;
            }
            g_pmm.free_page_locked(phys);
        }
    }

    // CoW fault (P=1, W/R=1, U/S=1)
    if ((err & 0x01) && (err & 0x02) && (err & 0x04)) {
        if (cinux::proc::handle_cow_fault(fault_addr)) return;
    }

    // Fatal error: dump_registers + kpanic ...
}
```

处理顺序：先 stack guard detection（两层：scheduler task + boot stack），再 demand-paging（P=0），再 CoW（P=1,W=1,U=1），最后 fatal。demand-paging 使用 alloc_page_locked 和 map_nolock——因为 #PF handler 在 Interrupt gate 下运行（IF=0），不能拿自旋锁（递归 #PF 会死锁）。如果 map_nolock 失败，使用 free_page_locked 释放已分配的物理页。CoW 检测条件用三个位同时置位来区分。如果 handle_cow_fault 返回 true，直接 return，不走到 fatal error。

map_flags 的 FLAG_USER 位根据 is_user_vaddr(fault_addr) 动态决定。如果 fault 地址在用户空间范围内（低于 KERNEL_VMA），添加 FLAG_USER；否则不加。这确保了内核空间和用户空间的页有不同的权限。

### context_switch.S — GS MSR 保存

```asm
    /* Save GS MSRs */
    movq $0xC0000101, %rcx
    rdmsr
    movl %eax, 64(%rdi)    /* from+64: gs_base low 32 */
    movl %edx, 68(%rdi)    /* from+68: gs_base high 32 */

    movq $0xC0000102, %rcx
    rdmsr
    movl %eax, 72(%rdi)    /* from+72: kgs_base low 32 */
    movl %edx, 76(%rdi)    /* from+76: kgs_base high 32 */
```

MSR_GS_BASE (0xC0000101) 和 MSR_KERNEL_GS_BASE (0xC0000102) 是 x86_64 的 64 位 MSR。rdmsr 把 64 位值返回在 EDX:EAX（各 32 位），我们用 movl 分别存到 CpuContext 的 gs_base 和 kgs_base 字段。注意 movl 而不是 movq——因为 rdmsr 只修改 EAX 和 EDX（各 32 位），直接用 32 位 store 避免覆盖相邻字段的高位。offset 64 和 68 分别对应 gs_base 的低 32 位和高 32 位，offset 72 和 76 分别对应 kgs_base 的低 32 位和高 32 位。

### context_switch.S — GS MSR 恢复

```asm
    /* Restore GS MSRs */
    movl 64(%rsi), %eax
    movl 68(%rsi), %edx
    movq $0xC0000101, %rcx
    wrmsr

    movl 72(%rsi), %eax
    movl 76(%rsi), %edx
    movq $0xC0000102, %rcx
    wrmsr
```

恢复是保存的逆操作：从 to 的 CpuContext 中读出低 32 位和高 32 位，拼入 EAX/EDX，然后 wrmsr 写入对应的 MSR。这保证了每个任务恢复执行时，GS MSR 处于它被切出时的状态——无论当时 swapgs 对是否完成。

CpuContext 的 static_assert 验证了布局正确性：gs_base 在 offset 64，kgs_base 在 offset 72，总大小 80 字节（之前是 64 字节）。扩展后 fork() 和 TaskBuilder::build() 中的初始化也需要填充这两个字段。

## 设计决策

### 决策：#PF handler 中使用无锁路径

**问题**: #PF handler 需要分配物理页和映射页表，但这些操作通常需要锁。在 IF=0 下拿锁会死锁吗？
**做法**: 使用 PMM 的 alloc_page_locked 和 VMM 的 map_nolock。
**原因**: #PF handler 在 Interrupt gate 下运行（IF=0），同 CPU 不可能有并发访问。如果触发递归 #PF（比如分配页表时缺页），当前 CPU 已经在 #PF handler 中，不会重入。但 VMM 的自旋锁假设了 IF=1（spinlock 内部会 sti），所以在 IF=0 下拿锁会立刻死锁。无锁路径绕过了这个问题。

## 扩展方向

- 引用计数 CoW（记录物理页共享数，减到 1 时提前清除 CoW 标记避免不必要的 fault）
- soft-dirty PTE tracking（标记自 fork 以来被写入的页，用于增量备份）
- #PF handler IST 栈支持（防止栈溢出时递归 #PF 导致 Double Fault）

## 参考资料
- Intel SDM Vol.3A Section 6.15 "Interrupt 14—Page-Fault Exception (#PF)"
- Intel SDM Vol.3A Section 5.8.4 "SWAPGS Instruction"
- OSDev Wiki #PF: https://wiki.osdev.org/Exceptions#Page_Fault
