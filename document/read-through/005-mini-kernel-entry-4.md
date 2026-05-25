---
title: 005-mini-kernel-entry-4 · 内核入口
---

# 005 通读版 · Host 端测试与测试自动化——从手动到自动

## 概览

上篇文章讲了内核端测试——在 QEMU 里验证 C++ 运行时特性（构造/析构、虚函数、多重继承）。但格式化函数（`format_decimal`、`format_hex`、`format_binary`）是纯算法代码，不依赖任何硬件，每次都启动 QEMU 来测试它们实在太慢了。这一篇讲 Host 端测试的构建方式、自研测试框架的实现细节，以及把 Host 测试和内核测试统一到一个 `make test` 入口的自动化流程。

本文覆盖 Host 端格式化测试（`test/unit/test_kprintf.cpp`）、自研测试框架（`test/framework/test_framework.h`）、Host 测试构建配置（`test/CMakeLists.txt`）、测试运行脚本（`scripts/run_all_tests.sh.in`）、QEMU 调试/测试目标配置（`cmake/qemu.cmake`），以及相关的构建配置变更。

## Host 端格式化测试——`test/unit/test_kprintf.cpp`

Host 端测试直接 include 内核的格式化函数头文件，在 Linux 用户态下运行：

```cpp
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <limits.h>
#include <cstdint>
#include <string>

#include "mini/lib/private/format.h"

using namespace cinux::mini::lib::detail;
```

`#define TEST_FRAMEWORK_IMPL` 引入自研测试框架的实现（不是只引入声明），然后通过 `#include "mini/lib/private/format.h"` 直接引用内核的格式化函数。注意这里没有 `#include "mini/lib/kprintf.h"`——测试只关注底层的格式化算法，不涉及串口输出。

```cpp
TEST("kprintf: decimal positive") {
    char buffer[64];
    int  len = format_decimal(42, buffer, sizeof(buffer));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(std::string(buffer), "42");
}

TEST("kprintf: decimal INT64_MIN") {
    char buffer[64];
    int  len = format_decimal(INT64_MIN, buffer, sizeof(buffer));
    ASSERT_EQ(len, 20);
    ASSERT_EQ(std::string(buffer), "-9223372036854775808");
}

TEST("kprintf: hex all digits") {
    char buffer[64];
    int  len = format_hex(0x123456789ABCDEF0ULL, buffer, sizeof(buffer), true);
    ASSERT_EQ(len, 16);
    ASSERT_EQ(std::string(buffer), "123456789abcdef0");
}

TEST("kprintf: binary max 64-bit") {
    char buffer[65];
    int  len = format_binary(0xFFFFFFFFFFFFFFFFULL, buffer, sizeof(buffer));
    ASSERT_EQ(len, 64);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(buffer[i], '1');
    }
}
```

这些测试覆盖了关键边界情况：`INT64_MIN`（负数绝对值溢出）、`0x123456789ABCDEF0`（完整 16 位十六进制）、`0xFFFFFFFFFFFFFFFF`（64 位全 1）。Host 端测试的优势在于可以直接用 `std::string` 做字符串比较——这在内核端是做不到的，因为 `std::string` 需要堆分配。

Host 测试能直接测试内核源文件（`format.cpp`），是因为 `format.cpp` 的代码是纯算法——不依赖串口、不依赖 I/O 端口、不依赖任何内核特有的基础设施。只要用 `-DCINUX_HOST_TEST` 编译，`format.cpp` 就能在 Linux 用户态正常编译和运行。这种"算法代码在 Host 和 Kernel 之间复用"的模式是 Cinux 测试架构的设计亮点。

## 自研 TEST() 宏框架——`test/framework/test_framework.h`

Host 端的测试框架放在一个头文件里，总共不到 200 行。核心机制是 `TEST()` 宏的自动注册——它通过 C++ 的全局构造函数在 `main()` 之前把测试用例登记到全局数组中，`RUN_ALL_TESTS()` 遍历这个数组执行所有测试。

```cpp
#define TEST(test_name) \
    static void _test_fn_##test_name(); \
    static struct _TestAutoReg_##test_name { \
        _TestAutoReg_##test_name() { \
            _register_test(#test_name, __FILE__, __LINE__, _test_fn_##test_name); \
        } \
    } _test_reg_##test_name; \
    static void _test_fn_##test_name()
```

`TEST()` 宏展开后做了三件事：声明一个唯一的测试函数（`_test_fn_##test_name`），创建一个静态注册对象（`_TestAutoReg_##test_name`），在注册对象的构造函数里把测试信息注册到全局数组中。静态对象的构造函数在 `main()` 之前执行，所以所有测试用例在程序启动时就已经注册好了。

断言方面提供了 `ASSERT_TRUE`、`ASSERT_FALSE`、`ASSERT_EQ`、`ASSERT_NE`、`ASSERT_NULL`、`ASSERT_NOT_NULL`、`ASSERT_GE`、`ASSERT_LE`、`ASSERT_GT`、`ASSERT_LT` 一整套。断言失败时打印测试名、失败的表达式、文件名和行号，然后 `return` 退出当前测试函数——这是 fail-fast 策略，避免级联错误。

平台适配层通过 `CINUX_HOST_TEST` 宏切换：定义了这个宏时，输出映射到 `printf`；未定义时，映射到内核的 `serial_printf`。使用时需要 `#define TEST_FRAMEWORK_IMPL` 然后包含这个头文件——这是一种单头文件库的常见模式，确保全局数组和函数只有一份定义。

## Host 测试构建——`test/CMakeLists.txt`

```cmake
set(CINUX_HOST_TEST ON)

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

enable_testing()
```

`enable_testing()` 启用 CMake 的 CTest 功能。`TEST_INCLUDE_DIRS` 把测试框架头文件和内核源文件目录都加到 include 路径中。

```cmake
add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)
```

`add_custom_target(test)` 定义了统一的 `make test` 目标，依赖 `test_smoke`（冒烟测试）、格式化测试和 `test-image`（测试磁盘映像）。运行顺序由脚本控制：先跑 Host 端 CTest，再跑 QEMU 内核测试。

## 测试运行脚本——`scripts/run_all_tests.sh.in`

```bash
#!/bin/sh
set -e
echo ''
echo '=== Running Host Tests ==='
@CMAKE_CTEST_COMMAND@ --output-on-failure
echo ''
echo '=== Running Kernel Tests ==='
set +e
@QEMU_EXECUTABLE@ @QEMU_COMMON_FLAGS_STR@ @QEMU_TEST_EXTRA_FLAGS_STR@ \
    -drive file=@CMAKE_BINARY_DIR@/cinux_test.img,format=raw,index=0,media=disk
QEMU_EXIT=$?
set -e
if [ $QEMU_EXIT -eq 1 ]; then
    echo "=== Kernel tests passed ==="
    exit 0
else
    echo "=== Kernel tests FAILED (exit code: $QEMU_EXIT) ===" >&2
    exit 1
fi
```

`set -e` 确保 Host 测试失败时不启动 QEMU。`set +e` 临时关闭退出检查，因为 QEMU 的退出码需要手动判断。`isa-debug-exit` 设备的退出码公式是 `(value << 1) | 1`：内核写入 0，QEMU 退出码为 1，表示测试通过。

## QEMU 调试/测试目标——`cmake/qemu.cmake`

```cmake
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)

add_custom_target(run-kernel-test
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (auto-exit)"
    VERBATIM
)
```

`isa-debug-exit` 设备只在测试相关的目标中使用。`run-kernel-test` 目标加载测试磁盘映像并启用自动退出。

值得注意的是，根目录的 `CMakeLists.txt` 中 `include(cmake/qemu.cmake)` 被移到了 `add_subdirectory(test)` 之前。原因是 `qemu.cmake` 中定义了 `run-kernel-test` 等测试目标，而 `test/CMakeLists.txt` 的 `add_custom_target(test)` 引用了这些目标。如果顺序反了，CMake 会报"目标未定义"的错误。

同样地，内核构建配置中 `MINI_KERNEL_COMMON_COMPILE_OPTIONS` 和 `MINI_KERNEL_COMMON_LINK_OPTIONS` 被 005 提取为公共变量，供生产内核和测试内核共享。在此之前编译选项是直接写在 `target_compile_options` 里的，提取成变量后任何修改只需改一处。

## 设计决策

### 决策：双模式测试 vs 纯内核测试

**问题**：内核代码的测试策略是什么？所有测试都在 QEMU 里跑，还是分 Host/Kernel 两种模式？

**问题**：内核代码的测试策略是什么？

**本项目的做法**：双模式。纯算法代码在 Host 端用 CTest 测试，涉及硬件和运行时的代码在 QEMU 内核模式测试。

**为什么选双模式**：Host 端测试有一个压倒性的优势——快。`ctest --output-on-failure` 在 100 毫秒内就能跑完。在 TDD 循环中，反馈速度直接决定开发效率。而且 Host 端可以用 GDB 单步调试、`std::string` 做断言比较——这些在 QEMU 环境下成本极高。

### 决策：自研测试框架 vs Google Test

**本项目的做法**：自研轻量框架，位于 `test/framework/test_framework.h`。核心是 `TEST()` 宏（自动注册）和 `ASSERT_*` 宏。

**为什么选自研**：Google Test 功能强大但很重——需要 `FetchContent` 或系统安装、链接额外的库、处理 `main()` 函数冲突。测试框架的复杂度不应该超过被测代码。自研框架不到 200 行，`TEST()` 宏在概念上和 gtest 的 `TEST()` 完全兼容，迁移成本很低。

### 决策：`isa-debug-exit` 自动退出 vs 手动 Ctrl+C

**本项目的做法**：QEMU 的 `isa-debug-exit` 设备 + 内核端 `outl $0xf4` 指令。

**为什么选 isa-debug-exit**：专门为测试设计的退出机制——简单、可靠、无歧义。退出码 `(value << 1) | 1` 可以区分不同退出原因。外层脚本根据退出码判断结果，完全自动化。

## 扩展方向

1. **CI/CD 集成**（难度：简单）——当前基础设施已支持 CI。只需 `.github/workflows/test.yml` 安装 QEMU 和工具链，运行 `cmake -DCINUX_BUILD_TESTS=ON`。

2. **回归测试基准**（难度：中等）——把预期输出保存为基准文件，用 QEMU `-serial file:` 重定向串口输出，自动比对。

3. **覆盖率收集**（难度：中等）——Host 端用 `gcov`/`lcov` 收集覆盖率。内核端更复杂，可后续考虑。

## 参考资料

- CMake Documentation — CTest (https://cmake.org/cmake/help/latest/manual/ctest.1.html)：CTest 命令行参数和配置选项，`--output-on-failure`、`-R`、`-L` 等。

- QEMU Documentation — isa-debug-exit (https://www.qemu.org/docs/master/system/gdb.html)：`isa-debug-exit` 的 I/O 端口配置和退出码计算公式。

- Linux Kernel KUnit (https://www.kernel.org/doc/html/latest/dev-tools/kunit/index.html)：Linux 内核的单元测试框架。Cinux 的自研框架是 KUnit 的极简版本。
