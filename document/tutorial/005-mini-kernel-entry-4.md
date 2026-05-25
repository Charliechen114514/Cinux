---
title: 005-mini-kernel-entry-4 · 内核入口
---

# 让内核自己说"我没问题"：双模式测试框架的搭建

> 标签：测试框架, TEST 宏, CTest, isa-debug-exit, 双模式测试, Host 测试, 内核测试, C++ 运行时验证
> 前置：[005C 踩过的两个大坑：PDPT 索引与链接脚本](005-mini-kernel-entry-3.md)

## 写在前面

前几篇我们写了串口驱动和 kprintf，修了两个 bug，内核终于能通过串口输出 `Cinux Mini Kernel v0.1.0` 了。但有一个问题一直在困扰我：我怎么知道这些代码确实是对的？

格式化函数 `format_decimal`、`format_hex`、`format_binary` 是纯算法，你可以手算几个测试用例在脑子里验证。但当用例越来越多——正数、负数、零、INT64_MIN、INT64_MAX、各种进制的边界值——人眼检查根本靠不住。更别提 C++ 运行时特性了：构造函数和析构函数的调用次数对不对？虚函数表在高半核地址空间中能不能正常工作？多重继承的 `this` 指针调整有没有问题？这些都需要在真实的内核环境中验证，不可能靠人脑推演。

手动重启 QEMU、看输出、逐行对比预期结果，重复 100 次之后我终于受不了了。tag 005 的最后一部分就是搭建一套自动化的测试基础设施，让内核能自己验证自己的行为是否正确——跑完全部测试后自动退出 QEMU，外层脚本检查退出码告诉你"通过"还是"失败"。

## 环境 & 背景

本篇涉及的代码文件比较多，但可以按功能分成三组。测试框架核心是 `test/framework/test_framework.h`（自研轻量框架的声明和实现），Host 端测试在 `test/unit/test_kprintf_format.cpp`（格式化函数单元测试）和 `test/CMakeLists.txt`（构建配置），内核端测试在 `kernel/mini/test/main_test.cpp`（测试入口）和 `kernel/mini/test/test_cpp_basic.cpp`（C++ 运行时测试）。自动化方面有 `scripts/run_all_tests.sh.in`（统一测试运行脚本）和 `cmake/qemu.cmake`（QEMU 测试目标配置）。

工具链方面新增了 CTest（CMake 内置的测试运行器）和 QEMU 的 `isa-debug-exit` 设备。构建时需要传 `-DCINUX_BUILD_TESTS=ON` 来启用测试目标。

## 为什么内核也需要测试框架

如果你之前只做过用户态开发，你可能会问：内核里能跑测试吗？毕竟内核运行在 QEMU 模拟的裸机环境里——没有标准库、没有 `main()` 的标准入口、没有 `assert()`、没有 Google Test，甚至连 `printf` 都没有（好吧，现在我们有了 kprintf，但那是刚刚才写出来的）。

答案是：当然能跑，只是需要做一些适配。测试的本质就是"执行一段代码，检查结果是否符合预期"——这在任何环境下都能做。关键是怎么把"执行"和"检查"这两个步骤在裸机环境里实现。

Cinux 的做法是把测试分成两种模式。Host 模式在 Linux 用户态直接运行，用标准 `assert` 和 `printf`——验证纯算法逻辑（比如格式化函数）。内核模式编译为独立的测试内核，通过 QEMU 启动后在真实的内核环境中运行——验证 C++ 运行时特性（构造/析构、虚函数、多重继承）。两种模式通过 CMake/CTest 统一调度，一条 `make test` 命令跑完全部测试。

## Host 端测试：在 Linux 上验证纯算法

Host 端测试的设计思路很直接：格式化函数（`format_decimal`、`format_hex`、`format_binary`）是纯算法代码——不依赖串口、不依赖 I/O 端口、不依赖任何内核特有的基础设施。只要用 `-DCINUX_HOST_TEST` 宏编译，这些代码就能在 Linux 用户态正常编译和运行。

为什么不在内核环境里测格式化函数？因为 Host 端测试有一个压倒性的优势——快。`ctest --output-on-failure` 在 100 毫秒内就能跑完，不需要启动 QEMU、不需要等 bootloader 执行、不需要等内核初始化。在 TDD（测试驱动开发）的循环中，"写代码 -> 跑测试 -> 看结果 -> 改代码"的反馈速度直接决定了开发效率。而且 Host 端测试可以用 GDB 单步调试、用 `std::string` 做字符串比较——这些工具在 QEMU 环境下要么不可用，要么使用成本极高。

Host 测试直接 include 了内核的格式化函数头文件：

```cpp
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST
#include <string>
#include "mini/lib/private/format.h"

using namespace cinux::mini::lib::detail;

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

这些测试覆盖了关键边界情况：`INT64_MIN`（负数绝对值溢出）、`0x123456789ABCDEF0`（完整 16 位十六进制）、`0xFFFFFFFFFFFFFFFF`（64 位全 1）、以及零值。Host 测试能直接用 `std::string` 做字符串比较——这在内核端是做不到的，因为 `std::string` 需要堆分配。

构建配置在 `test/CMakeLists.txt` 中，关键是把内核源文件和测试源文件一起编译：

```cmake
add_executable(test_kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp  # 内核实现
)
target_compile_definitions(test_kprintf_format PRIVATE CINUX_HOST_TEST)
add_test(NAME kprintf_format COMMAND test_kprintf_format)
```

`CINUX_HOST_TEST` 宏让测试代码中的条件编译分支生效，启用 `printf` 输出和标准库引用。`add_test` 把这个可执行文件注册到 CTest 中，`ctest` 就能自动运行它。

## 自研 TEST() 宏框架：200 行搞定

为什么不直接用 Google Test？对于 Cinux 这种教学 OS 项目来说，引入 Google Test 有点杀鸡用牛刀。Google Test 功能强大但很重——需要 CMake 的 `FetchContent` 或系统安装、链接额外的库、处理 `main()` 函数冲突。测试框架本身的复杂度不应该超过被测代码。

Cinux 的自研框架在 `test/framework/test_framework.h` 中，总共不到 200 行。核心是 `TEST()` 宏（自动注册测试用例）和 `ASSERT_*` 宏（断言检查）：

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

`TEST()` 宏做了三件事：声明一个唯一的测试函数（`_test_fn_##test_name`），创建一个静态注册对象（`_TestAutoReg_##test_name`），在注册对象的构造函数里把测试信息（名字、文件、行号、函数指针）注册到全局数组中。静态对象的构造函数在 `main()` 之前执行，所以所有 `TEST()` 定义的测试用例在程序启动时就已经注册好了。`RUN_ALL_TESTS()` 遍历这个全局数组，逐个执行测试函数。

`ASSERT_*` 宏在条件不满足时输出失败信息（包括条件表达式、文件名、行号），增加失败计数器，然后 `return` 退出当前测试函数。这是一种 fail-fast 策略——第一个断言失败后不再继续执行后续断言，避免级联错误。

断言的完整列表包括 `ASSERT_TRUE`、`ASSERT_FALSE`、`ASSERT_EQ`、`ASSERT_NE`、`ASSERT_NULL`、`ASSERT_NOT_NULL`、`ASSERT_GE`、`ASSERT_LE`、`ASSERT_GT`、`ASSERT_LT`——对于一个教学项目来说足够了。`TEST()` 宏在概念上和 Google Test 的 `TEST()` 完全兼容，将来如果需要切到 gtest，迁移成本很低。

## 内核端测试：在 QEMU 里验证 C++ 运行时

Host 测试只能验证纯算法逻辑。但有些代码只能在内核环境测试：全局构造函数依赖 `.init_array` 段和 bootloader 的初始化流程，虚函数依赖 vtable 在高半核地址空间中的正确映射，串口发送依赖 I/O 端口权限（Ring 0）。这些只有在 QEMU 里跑真实的内核才能验证。

内核端测试编译为独立的测试内核（`mini_kernel_test`），共享和生产内核完全相同的编译选项（`-ffreestanding`、`-fno-exceptions`、`-fno-rtti`、`-mcmodel=large`、`-mno-red-zone`）。这一点至关重要：如果测试通过但生产内核崩溃，你可以排除"编译选项差异导致行为不同"的可能性。

测试入口 `main_test.cpp` 首先做 kprintf 的冒烟测试——覆盖所有格式符，然后调用 `run_cpp_tests()` 执行 C++ 运行时测试：

```cpp
// test_cpp_basic.cpp 中的四个测试

// Test 1: 简单类构造/析构
// 验证构造函数在对象创建时调用，析构函数在作用域结束时调用

// Test 2: 虚函数
// 验证 vtable 机制：Base* base = &derived; base->getName() 分派到 Derived

// Test 3: 全局对象构造
// 验证 .init_array 机制：static 全局对象在 main 之前被构造

// Test 4: 多重继承
// 验证双 vtable 布局和 this 指针调整
```

内核端测试用了比 Host 端更简化的宏方案（`TEST_ASSERT` 和 `RUN_TEST`），因为不需要自动注册——直接在 `main_test.cpp` 里手动调用每个测试函数。这看似倒退，实际上是有意的简化：内核端测试要验证的正是全局构造函数机制，而 Host 端框架的自动注册恰恰依赖全局构造函数。如果内核端也用自动注册，那就是"用你要测试的机制来测试这个机制"——循环依赖。

四个测试用例分别验证 C++ 运行时的不同方面。Test 1 验证最基本的构造/析构调用次数。Test 2 验证虚函数表机制——`Base* base = &derived; base->getName()` 通过 vtable 分派到 `Derived::getName()`。Test 3 验证 `.init_array` 机制——如果链接脚本的 LMA 计算有误（上一篇讲的那个 bug），全局对象的构造函数根本不会被调用，测试就会失败。Test 4 验证多重继承的 vtable 布局——`Multi` 对象有两个 vtable 指针，`Base2* b2 = &m` 需要 this 指针调整。这些测试能通过，说明内核的代码段、数据段映射和 C++ 运行时支持都是正确的。

## isa-debug-exit：让 QEMU 自动退出

内核端测试有一个必须解决的问题：测试跑完之后怎么让 QEMU 退出？你不可能每次都手动 Ctrl+C——那就回到了"手动重启 QEMU 100 次"的老路。

QEMU 提供了一个专门为此设计的设备：`isa-debug-exit`。配置方式是在 QEMU 启动参数中加 `-device isa-debug-exit,iobase=0xf4,iosize=0x04`。内核向端口 0xF4 写入一个双字值，QEMU 就会以特定的退出码退出。退出码的计算公式是 `(value << 1) | 1`——所以写入 0 的退出码是 1，写入 1 的退出码是 3，以此类推。

在 `main_test.cpp` 的末尾：

```cpp
__asm__ volatile("outl %0, $0xf4" : : "a"(0));  // QEMU 退出码 = 1
```

外层的 `run_all_tests.sh` 脚本检查 QEMU 的退出码：

```bash
QEMU_EXIT=$?
if [ $QEMU_EXIT -eq 1 ]; then
    echo "=== Kernel tests passed ==="
    exit 0
else
    echo "=== Kernel tests FAILED (exit code: $QEMU_EXIT) ===" >&2
    exit 1
fi
```

如果内核中途崩溃（Page Fault、Triple Fault 等），QEMU 的退出码不会是 1——脚本据此判断测试失败。这个机制完全自动化了"内核测试 -> 退出 -> 判断结果"的流程。

## 统一测试入口：make test 一键跑完

最后把 Host 测试和内核测试统一到一个入口：

```cmake
add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)
```

`run_all_tests.sh` 的执行流程是：先跑 Host 端 CTest（`ctest --output-on-failure`），如果失败就不继续；然后启动 QEMU 跑内核端测试，检查退出码。整个过程一条 `make test` 命令搞定。

有一个构建配置的小细节值得一提。CMakeLists.txt 中 `include(cmake/qemu.cmake)` 的位置被调整到了 `add_subdirectory(test)` 之前。原因是 `cmake/qemu.cmake` 定义了 `run-kernel-test` 等目标，而 `test/CMakeLists.txt` 的 `add_custom_target(test)` 引用了这些目标。如果顺序反了，CMake 会报"目标未定义"的错误。

## 与 Linux KTest / xv6 测试方式的对比

Linux 内核有自己的测试基础设施——KUnit。KUnit 是一个内核内的单元测试框架，测试代码编译为内核模块，加载后在内核环境中运行。它有自己的断言宏（`KUNIT_EXPECT_EQ`、`KUNIT_ASSERT_NOT_ERR_OR_NULL` 等），支持参数化测试和测试套件，测试结果通过 debugfs 导出。KUnit 的设计理念和 Cinux 的自研框架类似——"测试代码在内核环境中运行"——但规模大了几个数量级。

xv6 的测试方式更朴素。它没有一个正式的测试框架，而是写了一系列用户态程序（`usertests.c`），在 xv6 启动后自动运行这些程序，检查它们的输出和退出状态。这种方式的好处是测试的是完整的系统行为（从系统调用到文件系统），坏处是粒度太粗——你不知道是哪个具体的子系统出了问题。Cinux 的内核端测试更接近 KUnit 的理念：在内核内部直接测试特定功能，而不是通过用户态程序间接测试。

三种测试策略代表了对"内核测试"这个问题的不同解法。Cinux 的双模式架构（Host 端测算法 + 内核端测运行时）在简单性和覆盖率之间取了一个平衡点。Host 端测试提供了快速反馈（毫秒级），内核端测试提供了真实环境验证。两者互补，缺一不可。

## 收尾

来跑一下全部测试验证成果：

```bash
cd build && cmake -DCINUX_BUILD_TESTS=ON .. && make -j$(nproc)
make test
```

你应该看到类似这样的输出：

```
=== Running Host Tests ===
Test project /home/user/cinux/build
    Start 1: smoke
1/2 Test #1: smoke ........................   Passed    0.00 sec
    Start 2: kprintf_format
2/2 Test #2: kprintf_format ................   Passed    0.00 sec

100% tests passed, 0 tests failed

=== Running Kernel Tests ===
[QEMU output...]
=== All tests completed ===
=== Kernel tests passed ===
```

从"手动重启 QEMU 100 次人眼检查输出"到"一条 `make test` 自动跑完所有测试并报告结果"——这就是测试基础设施的价值。它不仅节省了时间，更重要的是给了你信心：每次修改代码后跑一遍测试，全绿就说明没有引入回归。这种信心在做大型重构时尤其珍贵。

tag 005 到此全部完成。我们给内核装上了嘴巴（串口驱动和 kprintf），修了两个幽灵 bug（PDPT 索引和链接脚本 LMA），搭了一套自动化的测试框架。内核从"一个哑巴黑盒子"变成了"能说话、能被调试、能自我验证"的系统。后续的 tag 将在此基础上构建更复杂的内核功能——内存管理、中断处理、进程调度——但核心的开发基础设施（输出、调试、测试）已经就位了。

## 参考资料

- QEMU Documentation — isa-debug-exit (https://www.qemu.org/docs/master/system/gdb.html)：QEMU 内置调试设备文档。`isa-debug-exit` 的 I/O 端口配置（iobase、iosize），退出码计算公式 `(value << 1) | 1`。Cinux 的内核端测试通过这个设备实现自动化退出。

- CMake Documentation — CTest (https://cmake.org/cmake/help/latest/manual/ctest.1.html)：CTest 的命令行参数和配置选项。`--output-on-failure` 在测试失败时显示输出，`-R` 按名称过滤测试，`-L` 按标签过滤。Cinux 的 Host 端测试通过 CTest 调度。

- OSDev Wiki — Calling Global Constructors (https://wiki.osdev.org/Calling_Global_Constructors)：`.init_array` 段的布局和遍历方式。Cinux 的 Test 3（全局对象构造）直接验证了这一机制在高半核地址空间中的正确性——如果 `.init_array` 的 LMA 计算有误，全局构造函数就不会被调用。

- Linux Kernel KUnit (https://www.kernel.org/doc/html/latest/dev-tools/kunit/index.html)：Linux 内核的单元测试框架。支持断言宏、参数化测试、测试套件，测试结果通过 debugfs 导出。Cinux 的自研框架是 KUnit 的极简版本。

- xv6 `usertests.c` (https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/usertests.c)：xv6 的用户态测试程序。通过创建进程、写文件、系统调用等方式测试操作系统的完整功能。和 Cinux 的内核内测试是不同的策略。
