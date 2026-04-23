/**
 * @file kernel/arch/x86_64/interrupts.S
 * @brief ISR (Interrupt Service Routine) stubs for the big kernel
 *
 * Provides ISR stubs for all CPU exceptions configured in idt.cpp.
 *
 * Workflow for each stub:
 *   1. For exceptions without a hardware error code: push dummy 0
 *   2. Save all general-purpose registers (construct InterruptFrame)
 *   3. Pass %rsp as first argument to the C handler
 *   4. Call the C handler function
 *   5. Restore all general-purpose registers
 *   6. Remove error code from stack
 *   7. Return via IRETQ
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# ISR Stub macro: exception without hardware error code
# ============================================================
# Push a dummy error code 0 to keep the InterruptFrame layout
# uniform across all handlers.
# ============================================================

.macro ISR_NOERRCODE name handler
.global \name
.type \name, @function
\name:
    /* Push dummy error code 0 */
    pushq $0                          # $0→(%rsp): push dummy error code 0

    /* Save all general-purpose registers */
    pushq %rax                        # %rax→(%rsp): save RAX
    pushq %rbx                        # %rbx→(%rsp): save RBX
    pushq %rcx                        # %rcx→(%rsp): save RCX
    pushq %rdx                        # %rdx→(%rsp): save RDX
    pushq %rbp                        # %rbp→(%rsp): save RBP
    pushq %rsi                        # %rsi→(%rsp): save RSI
    pushq %rdi                        # %rdi→(%rsp): save RDI
    pushq %r8                         # %r8→(%rsp): save R8
    pushq %r9                         # %r9→(%rsp): save R9
    pushq %r10                        # %r10→(%rsp): save R10
    pushq %r11                        # %r11→(%rsp): save R11
    pushq %r12                        # %r12→(%rsp): save R12
    pushq %r13                        # %r13→(%rsp): save R13
    pushq %r14                        # %r14→(%rsp): save R14
    pushq %r15                        # %r15→(%rsp): save R15

    /*
     * Stack alignment padding.
     *
     * The CPU pushes 5 qwords (SS, RSP, RFLAGS, CS, RIP = 40 bytes)
     * before entering the ISR.  We push 17 qwords above (1 dummy error
     * code + 15 GPRs + 1 padding = 136 bytes).  Total: 176 bytes,
     * which is a multiple of 16.  The subsequent 'call' pushes 8 bytes,
     * giving the C handler the standard ABI entry alignment of RSP % 16 == 8.
     */
    pushq $0                          # alignment padding (8 bytes)

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 8(%rsp), %rdi                # (RSP+8)→%rdi: point past padding to r15

    /* Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* Remove alignment padding */
    addq $8, %rsp                     # $8→%rsp: skip alignment padding

    /* Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9: restore R9
    popq %r8                          # (%rsp)→%r8: restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Remove error code from stack */
    addq $8, %rsp                     # $8→%rsp: skip error code

    /* Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub macro: exception with hardware error code
# ============================================================
# The CPU has already pushed the error code, so no dummy push needed.
# ============================================================

.macro ISR_ERRCODE name handler
.global \name
.type \name, @function
\name:
    /* Save all general-purpose registers (error code already on stack) */
    pushq %rax                        # %rax→(%rsp): save RAX
    pushq %rbx                        # %rbx→(%rsp): save RBX
    pushq %rcx                        # %rcx→(%rsp): save RCX
    pushq %rdx                        # %rdx→(%rsp): save RDX
    pushq %rbp                        # %rbp→(%rsp): save RBP
    pushq %rsi                        # %rsi→(%rsp): save RSI
    pushq %rdi                        # %rdi→(%rsp): save RDI
    pushq %r8                         # %r8→(%rsp): save R8
    pushq %r9                         # %r9→(%rsp): save R9
    pushq %r10                        # %r10→(%rsp): save R10
    pushq %r11                        # %r11→(%rsp): save R11
    pushq %r12                        # %r12→(%rsp): save R12
    pushq %r13                        # %r13→(%rsp): save R13
    pushq %r14                        # %r14→(%rsp): save R14
    pushq %r15                        # %r15→(%rsp): save R15

    /*
     * Stack alignment padding.
     *
     * For error-code exceptions the CPU pushes 6 qwords
     * (SS, RSP, RFLAGS, CS, RIP, error = 48 bytes) before entering
     * the ISR.  We push 15 GPRs + 1 padding = 16 qwords (128 bytes).
     * Total: 176 bytes, a multiple of 16.  The 'call' then gives the
     * C handler the standard ABI entry alignment of RSP % 16 == 8.
     */
    pushq $0                          # alignment padding (8 bytes)

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 8(%rsp), %rdi                # (RSP+8)→%rdi: point past padding to r15

    /* Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* Remove alignment padding */
    addq $8, %rsp                     # $8→%rsp: skip alignment padding

    /* Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9: restore R9
    popq %r8                          # (%rsp)→%r8: restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Remove hardware error code from stack */
    addq $8, %rsp                     # $8→%rsp: skip CPU-pushed error code

    /* Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub instantiation
# ============================================================

# Exceptions without error code
ISR_NOERRCODE isr_de_stub,  handle_de       /* #DE(0):  Divide Error */
ISR_NOERRCODE isr_db_stub,  handle_db       /* #DB(1):  Debug */
ISR_NOERRCODE isr_nmi_stub, handle_nmi      /* NMI(2) */
ISR_NOERRCODE isr_bp_stub,  handle_bp       /* #BP(3):  Breakpoint */
ISR_NOERRCODE isr_of_stub,  handle_of       /* #OF(4):  Overflow */
ISR_NOERRCODE isr_br_stub,  handle_br       /* #BR(5):  BOUND Range */
ISR_NOERRCODE isr_ud_stub,  handle_ud       /* #UD(6):  Invalid Opcode */
ISR_NOERRCODE isr_nm_stub,  handle_nm       /* #NM(7):  Device Not Available */

# Exceptions with error code
ISR_ERRCODE   isr_df_stub,  handle_df       /* #DF(8):  Double Fault */
ISR_ERRCODE   isr_ts_stub,  handle_ts       /* #TS(10): Invalid TSS */
ISR_ERRCODE   isr_np_stub,  handle_np       /* #NP(11): Segment Not Present */
ISR_ERRCODE   isr_ss_stub,  handle_ss       /* #SS(12): Stack Fault */
ISR_ERRCODE   isr_gp_stub,  handle_gp       /* #GP(13): General Protection */
ISR_ERRCODE   isr_pf_stub,  handle_pf       /* #PF(14): Page Fault */

# ============================================================
# Hardware IRQ stubs (PIC remapped vectors 0x20-0x2F)
# ============================================================
# Master PIC: IRQ0-7 → INT 0x20-0x27
# Slave PIC:  IRQ8-15 → INT 0x28-0x2F
#
# For milestone 014, IRQ0 (PIT) and IRQ1 (Keyboard) have real handlers.
# All other IRQs get a default no-op handler for safety.
# Note: The C handlers are responsible for PIC::send_eoi().
# ============================================================

/* Master PIC IRQs */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  keyboard_irq1_handler /* IRQ1(0x21): Keyboard */
ISR_NOERRCODE irq2_stub,  irq_default_handler   /* IRQ2(0x22): Cascade */
ISR_NOERRCODE irq3_stub,  irq_default_handler   /* IRQ3(0x23): COM2 */
ISR_NOERRCODE irq4_stub,  irq_default_handler   /* IRQ4(0x24): COM1 */
ISR_NOERRCODE irq5_stub,  irq_default_handler   /* IRQ5(0x25): LPT2 */
ISR_NOERRCODE irq6_stub,  irq_default_handler   /* IRQ6(0x26): Floppy */
ISR_NOERRCODE irq7_stub,  irq_default_handler   /* IRQ7(0x27): LPT1 */

/* Slave PIC IRQs */
ISR_NOERRCODE irq8_stub,  irq_default_handler   /* IRQ8(0x28):  RTC */
ISR_NOERRCODE irq9_stub,  irq_default_handler   /* IRQ9(0x29):  Free */
ISR_NOERRCODE irq10_stub, irq_default_handler   /* IRQ10(0x2A): Free */
ISR_NOERRCODE irq11_stub, irq_default_handler   /* IRQ11(0x2B): Free */
ISR_NOERRCODE irq12_stub, mouse_irq12_handler   /* IRQ12(0x2C): PS/2 Mouse */
ISR_NOERRCODE irq13_stub, irq_default_handler   /* IRQ13(0x2D): FPU */
ISR_NOERRCODE irq14_stub, irq_default_handler   /* IRQ14(0x2E): Primary ATA */
ISR_NOERRCODE irq15_stub, irq_default_handler   /* IRQ15(0x2F): Secondary ATA */
