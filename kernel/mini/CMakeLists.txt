# ==============================================================
# Cinux Mini Kernel - Minimal Build
# ==============================================================

# ==============================================================
# Mini Kernel Common Compile Options
# ==============================================================
# 确保生产内核和测试内核使用完全一致的编译选项
set(MINI_KERNEL_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)

set(MINI_KERNEL_COMMON_LINK_OPTIONS
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)

# ==============================================================
# Shared Object Library - 源文件只在一处维护
# ==============================================================

add_library(mini_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    driver/serial.cpp
    lib/kprintf.cpp
    lib/private/format.cpp
    mm/pmm.cpp
)

# 对象库的编译选项设为 PUBLIC，继承者自动获得
target_compile_options(mini_kernel_common PUBLIC ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})

# 对象库的 include 目录设为 PUBLIC
# CMAKE_SOURCE_DIR 是项目根目录，这样可以找到 boot/ 下的头文件
target_include_directories(mini_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/boot
)

# ==============================================================
# Production Kernel - 只包含独有的 main.cpp
# ==============================================================

add_executable(mini_kernel
    main.cpp
)

# 链接共享对象库（添加对象文件）
target_sources(mini_kernel PRIVATE $<TARGET_OBJECTS:mini_kernel_common>)

# 链接对象库以继承 PUBLIC 属性（编译选项、include 目录等）
target_link_libraries(mini_kernel PRIVATE mini_kernel_common)

# Set output directory to build/kernel/mini/
set_target_properties(mini_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel"
)

target_link_options(mini_kernel PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

# Post-build: Convert ELF to flat binary
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)

# ==============================================================
# Subdirectories
# ==============================================================

# Library build
add_subdirectory(lib)

# Tests (only when CINUX_BUILD_TESTS is ON)
if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()
