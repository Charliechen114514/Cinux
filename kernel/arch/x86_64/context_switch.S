/**
 * @file kernel/arch/x86_64/context_switch.S
 * @brief Cooperative context switch for kernel threads
 *
 * Saves callee-saved registers of the outgoing task into the `from`
 * CpuContext, restores callee-saved registers from the `to` CpuContext,
 * and jumps to the saved rip of `to`.
 *
 * CpuContext layout (must match process.hpp):
 *   Offset 0:  r15
 *   Offset 8:  r14
 *   Offset 16: r13
 *   Offset 24: r12
 *   Offset 32: rbp
 *   Offset 40: rbx
 *   Offset 48: rsp
 *   Offset 56: rip
 *
 * Calling convention (System V AMD64 ABI):
 *   %rdi = from (CpuContext* of outgoing task)
 *   %rsi = to   (CpuContext* of incoming task)
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# Function: context_switch
# Purpose: save callee-saved state of current task, restore next
# Input:   %rdi = from (CpuContext*), %rsi = to (CpuContext*)
# Output:  none (returns via restored rip)
# Clobbers: flags
# ============================================================
.global context_switch
.type context_switch, @function
context_switch:

    /* ---- Save callee-saved registers into `from` ---- */

    movq %r15, 0(%rdi)               # %r15→from+0: save R15
    movq %r14, 8(%rdi)               # %r14→from+8: save R14
    movq %r13, 16(%rdi)              # %r13→from+16: save R13
    movq %r12, 24(%rdi)              # %r12→from+24: save R12
    movq %rbp, 32(%rdi)              # %rbp→from+32: save RBP
    movq %rbx, 40(%rdi)              # %rbx→from+40: save RBX

    /* Save current RSP (stack pointer) */
    movq %rsp, 48(%rdi)              # %rsp→from+48: save RSP

    /* Compute the resume address and save as RIP */
    leaq .restore(%rip), %rax        # .restore address→%rax: compute return point
    movq %rax, 56(%rdi)              # %rax→from+56: save resume RIP

    /* ---- Restore callee-saved registers from `to` ---- */

    movq 0(%rsi), %r15               # to+0→%r15: restore R15
    movq 8(%rsi), %r14               # to+8→%r14: restore R14
    movq 16(%rsi), %r13              # to+16→%r13: restore R13
    movq 24(%rsi), %r12              # to+24→%r12: restore R12
    movq 32(%rsi), %rbp              # to+32→%rbp: restore RBP
    movq 40(%rsi), %rbx              # to+40→%rbx: restore RBX

    /* Switch to the new task's stack */
    movq 48(%rsi), %rsp              # to+48→%rsp: switch stack pointer

    /* Jump to the new task's saved instruction pointer */
    jmp *56(%rsi)                    # to+56→RIP: resume execution

.restore:
    /* Execution resumes here when this task is switched back in */
    ret                               # return to caller of context_switch
