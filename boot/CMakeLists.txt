# ==============================================================
# Cinux Bootloader CMake Configuration
# ==============================================================

# ==============================================================
# Common Boot Utilities
# ==============================================================

# Compile common/serial.S as an object file for inclusion
add_library(boot_common OBJECT
    common/serial.S
)

target_compile_options(boot_common PRIVATE
    -Wa,--32                    # Generate 32-bit object (can be linked as 16-bit)
)

# ==============================================================
# MBR (Master Boot Record)
# ==============================================================

add_executable(mbr
    mbr.S
)

# Assemble 16-bit code as 32-bit objects, link as 32-bit ELF
target_compile_options(mbr PRIVATE
    -Wa,--32                    # Assemble as 32-bit (allows 16-bit code)
)

target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386             
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie                    
)

# ==============================================================
# Stage2 Bootloader
# ==============================================================

add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>  # Include common functions
)

target_compile_options(stage2 PRIVATE
    -Wa,--32                    # Assemble as 32-bit (allows 16-bit code)
)

target_link_options(stage2 PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
    -nostdlib
    -no-pie
)

# ==============================================================
# Linker Scripts
# ==============================================================

# Create MBR linker script
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : {
        *(.text)
        *(.rodata)
    }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")

# Create Stage2 linker script
# Note: Stage2 is loaded by MBR to 0x8000 (see mbr.S:22 STAGE2_LOAD_ADDR)
# The link address MUST match the load address for absolute addressing to work.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
"
OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x8000;
    .text : {
        *(.text)
    }

    .gdt ALIGN(8) : {
        *(.gdt)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")

# ==============================================================
# Post-Build: Convert to Raw Binary
# ==============================================================

# MBR binary
add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
    VERBATIM
)

# Stage2 binary
add_custom_command(
    TARGET stage2
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:stage2> $<TARGET_FILE_DIR:stage2>/stage2.bin
    COMMENT "Converting Stage2 to raw binary: stage2.bin"
    VERBATIM
)