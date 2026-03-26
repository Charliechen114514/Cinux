# ==============================================================
# Cinux Mini Kernel - Test Build
# 只包含测试独有的源文件，共享的从 mini_kernel_common 继承
# ==============================================================

add_executable(mini_kernel_test
    main_test.cpp           # 测试专用 main
    test_cpp_basic.cpp
    test_pmm.cpp            # PMM tests (006)
)

# 链接共享对象库（添加对象文件）
target_sources(mini_kernel_test PRIVATE $<TARGET_OBJECTS:mini_kernel_common>)

# 链接对象库以继承 PUBLIC 属性（编译选项、include 目录等）
target_link_libraries(mini_kernel_test PRIVATE mini_kernel_common)

# 使用与生产内核完全相同的链接选项
target_link_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

set_target_properties(mini_kernel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel_test"
)

add_custom_command(TARGET mini_kernel_test
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kernel/mini
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:mini_kernel_test> ${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel_test.bin
    COMMENT "Converting mini kernel test to flat binary"
    VERBATIM
)
