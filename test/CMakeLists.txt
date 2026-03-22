# =============================================================================
# test/CMakeLists.txt
# @brief Cinux 测试配置
#
# 此文件配置 Cinux 的 host 端单元测试。
# 测试在宿主机（Linux）上运行，使用 -DCINUX_HOST_TEST 编译标志。
#
# 测试文件位置：
#   - test/unit/test_*.cpp : 单元测试
#   - test/integration/*.cpp : 集成测试（后续）
#
# 测试框架：
#   - test/framework/test_framework.h : 自研轻量测试框架
# =============================================================================

# ============================================================
# 测试编译选项
# ============================================================

# 设置测试专用编译定义
# 添加 CINUX_HOST_TEST 宏，告诉代码在 host 端运行
set(CINUX_HOST_TEST ON)

# 设置测试包含目录
# 添加测试框架和内核头文件目录
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

# ============================================================
# CTest 配置
# ============================================================

# 启用 CMake 的 CTest 功能
enable_testing()

# ============================================================
# 测试可执行文件：test_smoke
# ============================================================

# 创建冒烟测试可执行文件
# 这是最基础的测试，验证测试框架本身工作正常
add_executable(test_smoke
    unit/test_smoke.cpp
)

# 添加编译定义：CINUX_HOST_TEST
target_compile_definitions(test_smoke PRIVATE CINUX_HOST_TEST)

# 设置包含目录
target_include_directories(test_smoke PRIVATE ${TEST_INCLUDE_DIRS})

# 注册测试到 CTest
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES LABELS "smoke")
# ============================================================
# 后续测试占位（Milestone 2+）
# ============================================================

# 预留测试列表：
#   - test_kprintf : kprintf 格式化输出测试
#   - test_pmm : 物理内存管理器测试
#   - test_vmm : 虚拟内存管理器测试
#   - test_heap : 堆分配器测试
#   - test_keyboard : 键盘扫描码转换测试
#
# 每个测试的添加方式（示例）：
#   add_executable(test_kprintf unit/test_kprintf.cpp)
#   target_compile_definitions(test_kprintf PRIVATE CINUX_HOST_TEST)
#   target_include_directories(test_kprintf PRIVATE ${TEST_INCLUDE_DIRS})
#   add_test(NAME kprintf COMMAND test_kprintf)

# ============================================================
# 测试辅助目标
# ============================================================

# 创建测试运行辅助目标
# 目标：
#   - test : 运行所有测试（通过 CTest）
#   - test_verbose : 详细模式运行测试

add_custom_target(test
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke
    COMMENT "Running all tests..."
)

add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS test_smoke
    COMMENT "Running all tests in verbose mode..."
)

# 单个测试快捷方式
add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
