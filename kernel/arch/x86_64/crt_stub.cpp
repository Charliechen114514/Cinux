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
 *   - operator new / delete: redirected to Heap::alloc / Heap::free
 *
 * All stubs that represent programming errors simply cli;hlt forever.
 */

#include <stdint.h>
#include <stddef.h>
#include <new>

#include "kernel/mm/heap.hpp"

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
// Guard Variables for Function-Local Statics
// ============================================================

/**
 * @brief Acquire guard for function-local static initialization
 *
 * In a freestanding environment, the compiler generates calls to
 * these functions when a function-local static needs thread-safe
 * initialization.  On a single-core kernel we check/set the guard
 * atomically without any locking.
 *
 * The guard variable is a 64-bit value:
 *   - 0: not yet initialized
 *   - 1: initialization complete
 *
 * @return 1 if initialization should proceed, 0 if already done
 */
int __cxa_guard_acquire(uint64_t* guard) {
    if (*guard != 0) {
        return 0;
    }
    return 1;
}

/**
 * @brief Release guard after function-local static initialization
 *
 * Marks the guard as initialized so subsequent calls to
 * __cxa_guard_acquire return 0 (already initialized).
 */
void __cxa_guard_release(uint64_t* guard) {
    *guard = 1;
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
// Operator new / delete -- redirected to Heap
// ============================================================
// Must be outside extern "C" -- they need C++ mangling.

/**
 * @brief Single-object new -- delegates to Heap::alloc
 */
void* operator new(unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}

/**
 * @brief Array new -- delegates to Heap::alloc
 */
void* operator new[](unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}

/**
 * @brief Aligned new -- delegates to Heap::alloc with alignment
 */
void* operator new(unsigned long size, std::align_val_t align) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size),
                                   static_cast<size_t>(align));
}

/**
 * @brief Aligned array new -- delegates to Heap::alloc with alignment
 */
void* operator new[](unsigned long size, std::align_val_t align) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size),
                                   static_cast<size_t>(align));
}

/**
 * @brief Single-object delete -- delegates to Heap::free
 */
void operator delete(void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

/**
 * @brief Sized delete -- delegates to Heap::free (size ignored)
 */
void operator delete(void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);
}

/**
 * @brief Array delete -- delegates to Heap::free
 */
void operator delete[](void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

/**
 * @brief Sized array delete -- delegates to Heap::free (size ignored)
 */
void operator delete[](void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);
}

/**
 * @brief Aligned delete -- delegates to Heap::free (alignment ignored)
 */
void operator delete(void* ptr, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);
}

/**
 * @brief Aligned sized delete -- delegates to Heap::free
 */
void operator delete(void* ptr, unsigned long, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);
}
