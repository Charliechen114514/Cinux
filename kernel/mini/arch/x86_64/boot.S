/* ==============================================================
 * Cinux Mini Kernel - Bootstrap Entry Point
 * ==============================================================

 * This is the minimal entry point for the mini kernel.
 * Loaded by bootloader at physical address 0x200000 (2MB).
 * Virtual address is 0xFFFFFFFF80000000 (higher-half).
 */

.section .text
.code64

.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    cli

    /* Output '1' to debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9

    /* Setup stack - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Output '2' to debugcon - stack ready */
    movb $0x32, %al              /* '2' */
    outb %al, $0xE9

    /* Save BootInfo pointer BEFORE clearing BSS (BSS clear uses %rdi) */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi      /* destination */
    movq $__bss_end, %rcx        /* end */
    subq %rdi, %rcx              /* count = end - start */
    xorq %rax, %rax              /* value = 0 */
    rep stosb                    /* clear */

    /* Output '3' to debugcon - BSS cleared */
    movb $0x33, %al              /* '3' */
    outb %al, $0xE9

    /* Call global constructors (C++ runtime) */
    call _init_global_ctors

    /* Output '4' to debugcon - ctors done */
    movb $0x34, %al              /* '4' */
    outb %al, $0xE9

    /* Call C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

    /* Halt if kernel returns */
.halt:
    cli
    hlt
    jmp .halt

.size _start, .-_start

/* ==============================================================
 * BootInfo Pointer Storage (.data section - NOT .bss!)
 * ============================================================== */
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0

/* ==============================================================
 * Stack Section
 * ============================================================== */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
