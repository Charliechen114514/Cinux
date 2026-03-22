#
# kernel/CMakeLists.txt
#
# @brief Cinux 内核模块 CMake 配置
#
# 本文件配置内核的编译。在本 Milestone 中仅为占位，
# 实际的内核源文件将在后续 Milestone 添加。
#

# ============================================================
# 内核库目标（占位）
# ============================================================

# TODO: 创建内核库目标
#   思路: 使用 add_library(cinux_kernel STATIC) 创建静态库
#        或使用 add_executable(cinux_kernel ...) 创建可执行文件
#        本 Milestone 先创建接口库（INTERFACE）用于传递编译选项

# ============================================================
# 内核源文件（占位）
# ============================================================

# TODO: 为后续 Milestone 预留源文件添加位置
#   目录结构示例:
#     kernel/arch/x86_64/boot.S
#     kernel/arch/x86_64/crt_stub.cpp
#     kernel/lib/kprintf.cpp
#     kernel/drivers/serial.cpp
#   思路: 使用 target_sources() 逐步添加源文件

# ============================================================
# 内核头文件搜索路径
# ============================================================

# TODO: 设置内核头文件目录
#   思路: target_include_directories(cinux_kernel PUBLIC
#            ${CMAKE_CURRENT_SOURCE_DIR}
#            ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64
#            ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# ============================================================
# 链接器脚本（后续 Milestone）
# ============================================================

# TODO: 为链接器脚本预留位置
#   思路: 使用 target_link_options(cinux_kernel PRIVATE
#            -T${CMAKE_CURRENT_SOURCE_DIR}/linker.ld)

# ============================================================
# 内核依赖
# ============================================================

# TODO: 定义内核需要链接的库
#   思路: 目前为空，后续可能需要:
#        - gcc/g++ 的内置库（libgcc, libstdc++ 的 freestanding 部分）
#        - 自定义的内核库

# ============================================================
# 安装规则（可选）
# ============================================================

# TODO: 定义内核安装规则
#   思路: 将编译好的内核 ELF 复制到 build 目录
#        使用 install(TARGETS cinux_kernel DESTINATION build)
