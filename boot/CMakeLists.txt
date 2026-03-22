add_executable(mbr
    mbr_stub.S
)

target_link_options(mbr PRIVATE
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
)

# 链接后处理：转换为纯二进制格式
add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
    VERBATIM
)

# 创建 MBR 链接脚本
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf64-x86-64\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : { *(.text) }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")