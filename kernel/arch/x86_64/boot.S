/* ==============================================================
 * Cinux Big Kernel - Bootstrap Entry Point
 * ==============================================================

 * This is the very first code that executes when the mini kernel
 * jumps to the big kernel entry point.  The mini kernel has already
 * loaded our PT_LOAD segments into memory and parsed the ELF header,
 * so we land here in 64-bit long mode with identity-mapped pages.
 *
 * Responsibilities:
 *   1. Disable interrupts (we have no IDT set up yet)
 *   2. Set up a kernel stack (__kernel_stack_top from linker script)
 *   3. Clear the BSS section (zero out all uninitialised data)
 *   4. Run global constructors (.init_array)
 *   5. Call kernel_main(BootInfo*) -- the C++ entry point
 *   6. Halt if kernel_main ever returns (it should not)
 *
 * Calling convention:
 *   %rdi = pointer to BootInfo structure (passed by mini kernel)
 *
 * AT&T syntax note: operand order is "source, destination".
 * Register prefix %, immediate prefix $, memory offset(base,index,scale).
 */

.section .text.start, "ax"
.code64

.global _start
.type   _start, @function

_start:
    /* ----------------------------------------------------------
     * Step 1: Disable interrupts
     * ---------------------------------------------------------- */
    cli                           # disable interrupts

    /* ----------------------------------------------------------
     * Step 2: Set up the kernel stack
     * ----------------------------------------------------------
     * __kernel_stack_top is defined in linker.ld (16 KB above BSS).
     * We zero %rbp so stack walkers know this is the bottom frame.
     */
    movq  $__kernel_stack_top, %rsp  # rsp→stack top: set stack pointer
    xorq  %rbp, %rbp                # 0→rbp: clear base pointer (end of call chain)

    /* ----------------------------------------------------------
     * Step 3: Clear BSS section
     * ----------------------------------------------------------
     * The ELF loader may or may not have zeroed BSS for us,
     * depending on whether memsz > filesz in the PT_LOAD segment.
     * We do it unconditionally to be safe.
     *
     * Uses: %rdi = dest, %rcx = byte count, %rax = fill value (0)
     */
    movq  $__bss_start, %rdi       # &bss_start→rdi: destination address
    movq  $__bss_end, %rcx         # &bss_end→rcx: end address
    subq  %rdi, %rcx               # end-start→rcx: byte count
    xorq  %rax, %rax               # 0→rax: fill byte
    rep stosb                      # rax→[rdi]: zero-fill BSS, rcx times

    /* ----------------------------------------------------------
     * Step 4: Run global constructors (.init_array)
     * ----------------------------------------------------------
     * Calls _init_global_ctors() defined in crt_stub.cpp, which
     * iterates through the .init_array section and calls each
     * function pointer.  This is needed for C++ global objects
     * with non-trivial constructors.
     */
    call  _init_global_ctors

    /* ----------------------------------------------------------
     * Step 5: Call the C++ main function
     * ----------------------------------------------------------
     * %rdi should still contain the BootInfo pointer passed by
     * the mini kernel.  The BSS clear clobbered %rdi, but we
     * saved nothing -- BootInfo ptr is passed in %rdi at entry,
     * and crt_stub.cpp may or may not preserve it.
     *
     * TODO: If we need BootInfo, save %rdi before BSS clear and
     *       restore it here.  For milestone 009 we just call
     *       kernel_main without arguments (BootInfo* may be NULL).
     */
    xorq  %rdi, %rdi              # NULL→rdi: BootInfo* (unused for now)
    call  kernel_main

    /* ----------------------------------------------------------
     * Step 6: Halt (kernel_main should never return)
     * ---------------------------------------------------------- */
.halt:
    cli                           # disable interrupts
    hlt                           # halt processor
    jmp   .halt                   # loop→.halt: in case of NMI, re-halt

.size _start, . - _start
