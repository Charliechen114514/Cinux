/**
 * @file kernel/arch/x86_64/usermode.S
 * @brief Transition from kernel mode (Ring 0) to user mode (Ring 3)
 *
 * Provides jump_to_usermode() which uses the SYSRET instruction to enter
 * Ring 3.  Before the first call, usermode_init() must have been invoked
 * to configure the STAR MSR with the appropriate CS/SS selectors.
 *
 * SYSRET conventions (Intel SDM Vol. 2A):
 *   - RCX = target RIP (user entry point)
 *   - R11 = saved RFLAGS (restored into RFLAGS by SYSRET)
 *   - RSP must be set to the user stack before SYSRET
 *   - SYSRET loads RCX into RIP, R11 into RFLAGS
 *   - CS is set to STAR[48:63] + 16 (0x1B -> user code, Ring 3)
 *   - SS is set to STAR[48:63] + 8  (0x23 -> user data, Ring 3)
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# Function: usermode_init
# Purpose: configure STAR MSR for future SYSRET/SYSCALL transitions
# Input:   none
# Output:  none
# Clobbers: %rax, %rdx
# ============================================================
.global usermode_init_asm
.type usermode_init_asm, @function
usermode_init_asm:

    # Write STAR MSR (0xC0000081)
    #   [63:48] = SYSRET CS base selector (0x08, kernel code)
    #   [47:32] = SYSCALL CS base selector (0x08, kernel code)
    # The SYSRET target CS = STAR[63:48] + 16 = 0x18, which with RPL=3 = 0x1B
    # The SYSRET target SS = STAR[63:48] + 8  = 0x10, which with RPL=3 = 0x23
    movq $0x08, %rdx                   # $0x08→%rdx: kernel CS selector value
    shlq $16, %rdx                     # %rdx<<16→%rdx: shift to EDX[31:16] for STAR[63:48]
    orq $0x08, %rdx                    # $0x08|%rdx→%rdx: also set EDX[15:0] for STAR[47:32]
    xorq %rax, %rax                    # 0→%rax: clear low 32 bits (unused)
    movq $0xC0000081, %rcx             # MSR_STAR→%rcx: STAR MSR index
    wrmsr                              # %edx:%eax→MSR[rcx]: write STAR MSR

    # Set SFMASK MSR (0xC0000084) to mask IF on SYSCALL
    movq $0xC0000084, %rcx             # MSR_SFMASK→%rcx: SFMASK MSR index
    xorq %rdx, %rdx                    # 0→%rdx: high 32 bits = 0
    movq $0x200, %rax                  # $0x200→%rax: mask IF bit (bit 9)
    wrmsr                              # %edx:%eax→MSR[rcx]: write SFMASK

    # Enable SYSCALL/SYSRET via EFER.SCE (System Call Extensions)
    movq $0xC0000080, %rcx             # MSR_EFER→%rcx: EFER MSR index
    rdmsr                              # MSR[rcx]→%edx:%eax: read current EFER
    orq $1, %rax                       # $1|%rax→%rax: set SCE bit (bit 0)
    wrmsr                              # %edx:%eax→MSR[rcx]: write EFER with SCE enabled

    ret                                 # return to caller

# ============================================================
# Function: jump_to_usermode
# Purpose: transition from Ring 0 to Ring 3 via SYSRET
# Input:   %rdi = entry (user-space RIP)
#          %rsi = user_stack (user-space RSP)
#          %rdx = arg (first argument to user entry, passed via %rdi)
# Output:  does not return (enters user mode)
# Clobbers: all general-purpose registers
# ============================================================
.global jump_to_usermode
.type jump_to_usermode, @function
jump_to_usermode:

    # Set up SYSRET register convention
    # RCX = target RIP for SYSRET
    movq %rdi, %rcx                    # %rdi→%rcx: user entry point (SYSRET loads into RIP)

    # Set user stack pointer
    movq %rsi, %rsp                    # %rsi→%rsp: user stack pointer

    # First argument to user program (passed in %rdi at user entry)
    movq %rdx, %rdi                    # %rdx→%rdi: argument for user entry function

    # R11 = RFLAGS for SYSRET: set IF (bit 9) so interrupts are enabled in user mode
    pushq $0x202                       # $0x202→(%rsp): push RFLAGS with IF|bit1 set
    popq %r11                          # (%rsp)→%r11: pop into R11 (SYSRET restores this)

    # Zero out unused registers to avoid leaking kernel data
    xorq %rax, %rax                    # 0→%rax: clear RAX
    xorq %rbx, %rbx                    # 0→%rbx: clear RBX
    xorq %rdx, %rdx                    # 0→%rdx: clear RDX
    xorq %rsi, %rsi                    # 0→%rsi: clear RSI
    xorq %rbp, %rbp                    # 0→%rbp: clear RBP
    xorq %r8, %r8                      # 0→%r8:  clear R8
    xorq %r9, %r9                      # 0→%r9:  clear R9
    xorq %r10, %r10                    # 0→%r10: clear R10
    xorq %r12, %r12                    # 0→%r12: clear R12
    xorq %r13, %r13                    # 0→%r13: clear R13
    xorq %r14, %r14                    # 0→%r14: clear R14
    xorq %r15, %r15                    # 0→%r15: clear R15

    # Execute SYSRET to enter Ring 3
    # SYSRET: %rcx→RIP, %r11→RFLAGS, CS=STAR[63:48]+16|RPL, SS=STAR[63:48]+8|RPL
    sysretq                             # enter user mode (Ring 3)

.size jump_to_usermode, . - jump_to_usermode
