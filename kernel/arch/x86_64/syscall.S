/**
 * @file kernel/arch/x86_64/syscall.S
 * @brief SYSCALL entry point for Ring 3 -> Ring 0 transition
 *
 * When a user program executes the SYSCALL instruction:
 *   - RCX <- RIP (return address for SYSRETQ)
 *   - R11 <- RFLAGS
 *   - RIP <- LSTAR MSR value (syscall_entry below)
 *   - CS  <- STAR[47:32] (kernel code segment)
 *   - SS  <- STAR[47:32] + 8 (kernel data segment)
 *   - RFLAGS <- RFLAGS & ~SFMASK
 *
 * Syscall convention (Linux x86_64):
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   R10 = arg4, R8  = arg5, R9  = arg6
 *   RAX = return value
 *
 * Trap frame layout on kernel stack (pushed in order):
 *   rsp+0:  user RSP
 *   rsp+8:  user RIP (RCX)
 *   rsp+16: user RFLAGS (R11)
 *   rsp+24: syscall number (RAX) / return value after dispatch
 *   rsp+32: arg1 (RDI)
 *   rsp+40: arg2 (RSI)
 *   rsp+48: arg3 (RDX)
 *   rsp+56: arg4 (R10)
 *   rsp+64: arg5 (R8)
 *   rsp+72: arg6 (R9)
 *   rsp+80: callee-saved RBX
 *   rsp+88: callee-saved RBP
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# Function: syscall_entry
# Purpose: Ring 3 -> Ring 0 entry via SYSCALL instruction
# Input:   %rax = syscall number, %rdi-%r9 = arguments
#          %rcx = user RIP (saved by SYSCALL)
#          %r11 = user RFLAGS (saved by SYSCALL)
# Output:  %rax = return value (via SYSRETQ)
# Clobbers: none (all saved/restored)
# ============================================================
.global syscall_entry
.type syscall_entry, @function
syscall_entry:

    # Step 1: SWAPGS -- activate kernel GS base
    swapgs                              # kernel_gsbase↔user_gsbase: activate kernel GS

    # Step 2: Save user RSP to per-CPU scratch, load kernel stack
    movq %rsp, %gs:8                   # %rsp→%gs:8: save user RSP in scratch slot
    movq %gs:0, %rsp                   # %gs:0→%rsp: load kernel stack pointer

    # Step 3: Build trap frame on kernel stack
    pushq %rbp                         # %rbp→(%rsp): save callee-saved RBP
    pushq %rbx                         # %rbx→(%rsp): save callee-saved RBX
    pushq %r9                          # %r9→(%rsp): save arg6
    pushq %r8                          # %r8→(%rsp): save arg5
    pushq %r10                         # %r10→(%rsp): save arg4
    pushq %rdx                         # %rdx→(%rsp): save arg3
    pushq %rsi                         # %rsi→(%rsp): save arg2
    pushq %rdi                         # %rdi→(%rsp): save arg1
    pushq %rax                         # %rax→(%rsp): save syscall number
    pushq %r11                         # %r11→(%rsp): save user RFLAGS
    pushq %rcx                         # %rcx→(%rsp): save user RIP

    # Push user RSP (recovered from %gs:8)
    movq %gs:8, %rax                   # %gs:8→%rax: recover user RSP
    pushq %rax                         # %rax→(%rsp): save user RSP at frame top

    # Frame is now complete:
    #   rsp+0:  user_rsp
    #   rsp+8:  user_rip
    #   rsp+16: user_rflags
    #   rsp+24: syscall_nr
    #   rsp+32: rdi
    #   rsp+40: rsi
    #   rsp+48: rdx
    #   rsp+56: r10
    #   rsp+64: r8
    #   rsp+72: r9
    #   rsp+80: rbx
    #   rsp+88: rbp

    # Step 4: Call syscall_dispatch(nr, a1, a2, a3, a4, a5, a6)
    # System V AMD64: args in %rdi, %rsi, %rdx, %rcx, %r8, %r9, then stack
    # Load arg6 before pushing so offsets are correct
    movq 72(%rsp), %rax                # frame+72→%rax: a6 (original R9)
    pushq %rax                         # %rax→(%rsp): push 7th C arg onto stack (shifts frame by +8)

    movq 32(%rsp), %rdi                # frame+32→%rdi: 1st arg = syscall number (+8 offset)
    movq 40(%rsp), %rsi                # frame+40→%rsi: 2nd arg = a1 (original RDI)
    movq 48(%rsp), %rdx                # frame+48→%rdx: 3rd arg = a2 (original RSI)
    movq 56(%rsp), %rcx                # frame+56→%rcx: 4th arg = a3 (original RDX)
    movq 64(%rsp), %r8                 # frame+64→%r8:  5th arg = a4 (original R10)
    movq 72(%rsp), %r9                 # frame+72→%r9:  6th arg = a5 (original R8)

    call syscall_dispatch              # call C dispatcher

    addq $8, %rsp                      # $8→%rsp: remove 7th arg from stack

    # Step 5: Save return value in RBX (will not be touched by SYSRETQ)
    movq %rax, %rbx                    # %rax→%rbx: save return value

    # Step 6: Restore user state from trap frame
    movq 0(%rsp), %rax                 # frame+0→%rax: recover user RSP
    movq %rax, %gs:8                   # %rax→%gs:8: store user RSP for later restore
    movq 8(%rsp), %rcx                 # frame+8→%rcx: user RIP (SYSRETQ loads RIP from RCX)
    movq 16(%rsp), %r11                # frame+16→%r11: user RFLAGS (SYSRETQ loads RFLAGS from R11)

    # Deallocate trap frame (12 slots * 8 bytes = 96 bytes)
    addq $96, %rsp                     # $96→%rsp: remove entire trap frame

    # Step 7: Restore user RSP
    movq %gs:8, %rsp                   # %gs:8→%rsp: switch back to user stack

    # Step 8: Restore return value to RAX
    movq %rbx, %rax                    # %rbx→%rax: restore syscall return value

    # Step 9: SWAPGS back to user GS base
    swapgs                             # kernel_gsbase↔user_gsbase: activate user GS

    # Step 10: Return to Ring 3
    # SYSRETQ loads: RCX→RIP, R11→RFLAGS, CS=STAR[63:48]+16|RPL, SS=STAR[63:48]+8|RPL
    sysretq                            # return to Ring 3

.size syscall_entry, . - syscall_entry
