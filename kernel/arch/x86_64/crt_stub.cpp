/**
 * @file kernel/arch/x86_64/crt_stub.cpp
 * @brief Minimal C++ runtime support for the big kernel
 *
 * Provides the bare-minimum stubs that the compiler expects in a
 * freestanding (-ffreestanding -nostdlib) environment:
 *
 *   - __cxa_pure_virtual   : called if a pure virtual is invoked (bug)
 *   - __stack_chk_fail     : called if stack canary is corrupted
 *   - __cxa_atexit         : no-op (kernels never "exit")
 *   - _init_global_ctors   : walks .init_array, calls each constructor
 *   - operator new / delete: halt on use (no heap yet)
 *
 * All stubs that represent programming errors simply cli;hlt forever.
 */

#include <stdint.h>

extern "C" {

// ============================================================
// Pure Virtual Function Call Handler
// ============================================================

/**
 * @brief Invoked when a pure virtual function is called
 *
 * This should never happen in a correct kernel.  Halt immediately
 * so the developer notices during testing.
 */
[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================
// Stack Smashing Protector Failure Handler
// ============================================================

/**
 * @brief Invoked when the stack canary is corrupted
 *
 * With -fno-stack-protector this should never fire, but we
 * provide it anyway in case someone enables stack protectors.
 */
[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================
// Atexit Handler (no-op)
// ============================================================

/**
 * @brief Register an at-exit callback (no-op in a kernel)
 *
 * Kernels never terminate, so there is nothing to do.
 *
 * @return 0 (success -- we just ignore the registration)
 */
int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

// ============================================================
// Global Constructors
// ============================================================

/**
 * @brief Symbols provided by the linker script (.init_array section)
 */
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

/**
 * @brief Walk .init_array and invoke every global constructor
 *
 * Called from boot.S after BSS is cleared but before kernel_main().
 * Each entry in .init_array is a function pointer placed there by
 * the compiler for static/global objects with constructors.
 */
void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)()   = __init_array_end;

    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) {
            ctor();
        }
    }
}

}  // extern "C"

// ============================================================
// Operator new / delete (minimal stubs)
// ============================================================
// Must be outside extern "C" -- they need C++ mangling.
// Halt on use because the big kernel has no heap allocator yet.

/**
 * @brief Placement-like operator new -- no heap, halt on use
 */
void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

/**
 * @brief Array new -- no heap, halt on use
 */
void* operator new[](unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

/**
 * @brief Single-object delete -- no heap, halt on use
 */
void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

/**
 * @brief Sized delete -- no heap, halt on use
 */
void operator delete(void* ptr, unsigned long size) noexcept {
    (void)ptr;
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
