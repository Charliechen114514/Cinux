target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/crt_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/gdt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/idt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/interrupts.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/exception_handlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/irq_handlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/paging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/context_switch.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/usermode.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/usermode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall.cpp
)
