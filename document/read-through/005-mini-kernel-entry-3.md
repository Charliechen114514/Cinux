# 005 通读版 · 测试框架与调试工具链——内核也能做 TDD

## 概览

前两篇文章覆盖了串口驱动和 kprintf 格式化库——它们是内核的"嘴巴"。但光能说话还不够，我们还需要一套机制来验证内核代码是否正确工作。在用户态开发中，你可以用 Google Test、Catch2、甚至简单的 `assert()` 来验证逻辑。但在内核环境下，事情要复杂得多：标准库不可用、异常禁用、没有 `main()` 函数的标准入口、程序运行在 QEMU 模拟的裸机环境中。怎么在这样的条件下写测试？

Cinux 的答案是双模式测试架构：Host 端测试在 Linux 用户态直接运行，验证纯算法逻辑（比如格式化函数）；内核端测试编译为独立的测试内核，通过 QEMU 启动后在真实的内核环境中运行，验证 C++ 运行时特性（构造/析构、虚函数、多重继承）。两种模式通过 CMake/CTest 统一调度，一条 `make test` 命令跑完全部测试。内核端测试还能通过 QEMU 的 `isa-debug-exit` 设备自动退出，实现完全自动化的测试流程。

本文覆盖内核端测试构建配置（`kernel/mini/test/CMakeLists.txt`）和入口（`main_test.cpp`）、C++ 运行时测试用例（`test_cpp_basic.cpp`）、Host 端格式化单元测试（`test/unit/test_kprintf_format.cpp`）、Host 测试构建配置（`test/CMakeLists.txt`）、测试自动化脚本（`scripts/run_all_tests.sh.in` 和 `scripts/run_all_test_user_scripts.sh`）、QEMU 调试/测试目标配置（`cmake/qemu.cmake`）、根目录构建配置（`CMakeLists.txt`）、内核构建配置（`kernel/mini/CMakeLists.txt`），以及内核主函数中 kprintf 的使用（`main.cpp`）。

## 架构图

双模式测试架构的全景：

```
                Cinux 双模式测试架构
                =====================

  ┌──────────────────────────────────────────────────────────────┐
  │                     make test                                │
  │                    (统一入口)                                 │
  │                         │                                    │
  │          ┌──────────────┼──────────────┐                    │
  │          ▼                             ▼                    │
  │  ┌───────────────┐             ┌───────────────┐            │
  │  │  Host 端测试   │             │  内核端测试    │            │
  │  │  (Linux)      │             │  (QEMU)       │            │
  │  ├───────────────┤             ├───────────────┤            │
  │  │ test_smoke    │             │ main_test.cpp │            │
  │  │ test_kprintf_ │             │ test_cpp_     │            │
  │  │   format      │             │   basic.cpp   │            │
  │  ├───────────────┤             ├───────────────┤            │
  │  │ g++ / stdlib  │             │ -ffreestanding│            │
  │  │ CINUX_HOST_   │             │ same flags as │            │
  │  │   TEST 宏     │             │ production    │            │
  │  ├───────────────┤             │               │            │
  │  │ 自研测试框架   │             │ 简化 TEST_    │            │
  │  │ TEST() 宏     │             │   ASSERT 宏   │            │
  │  │ ASSERT_EQ 等  │             │ kprintf 输出  │            │
  │  └───────┬───────┘             ├───────────────┤            │
  │          │                     │ isa-debug-exit│            │
  │          │                     │ outl $0xf4    │            │
  │          │                     └───────┬───────┘            │
  │          │                             │                    │
  │          ▼                             ▼                    │
  │   CTest (ctest)                QEMU 退出码                  │
  │   通过/失败报告                1=成功, 其他=失败             │
  │                                                               │
  │         └──────────────┬──────────────┘                     │
  │                        ▼                                     │
  │               run_all_tests.sh                               │
  │               判断退出码 → 通过/失败                          │
  └──────────────────────────────────────────────────────────────┘
```

内核端测试的启动流程：

```
  Power On → MBR → Stage2 → Long Mode → 跳转测试内核
       │
       ▼
  _start (boot.S)
       ├── 设置栈、清 BSS、调用 _init_global_ctors
       └── call mini_kernel_main (main_test.cpp)
               │
               ├── kprintf 格式化测试
               ├── kdebugf 调试输出测试
               ├── run_cpp_tests()
               │       ├── test_simple_class (构造/析构)
               │       ├── test_virtual_functions (虚函数)
               │       ├── test_global_construction (全局对象)
               │       └── test_multiple_inheritance (多重继承)
               │
               └── outl $0xf4, %eax  → QEMU 自动退出
```

## 代码精讲

### 内核端测试构建——`kernel/mini/test/CMakeLists.txt`

```cmake
add_executable(mini_kernel_test
    ../arch/x86_64/boot.S
    ../arch/x86_64/crt_stub.cpp
    main_test.cpp
    ../driver/serial.cpp
    ../lib/kprintf.cpp
    ../lib/private/format.cpp
    test_cpp_basic.cpp
)

target_compile_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})

target_include_directories(mini_kernel_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

set_target_properties(mini_kernel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel_test"
)

add_custom_command(TARGET mini_kernel_test
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kernel/mini
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:mini_kernel_test>
            ${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel_test.bin
    COMMENT "Converting mini kernel test to flat binary"
    VERBATIM
)
```

测试内核和生产内核共享同一套编译选项 `${MINI_KERNEL_COMMON_COMPILE_OPTIONS}`（定义在父目录的 `CMakeLists.txt` 中，包含 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti`、`-mcmodel=large`、`-mno-red-zone`），以及同一套链接选项 `${MINI_KERNEL_COMMON_LINK_OPTIONS}`（包含 `-T linker.ld`、`-nostdlib`、`-no-pie`）。这一点至关重要：测试内核和生产内核运行在完全相同的环境约束下。如果测试通过但生产内核崩溃，你可以排除"编译选项差异导致行为不同"的可能性。

测试内核的源文件列表和生产内核几乎一样，区别在于用 `main_test.cpp` 替代了 `main.cpp`，并额外加入了 `test_cpp_basic.cpp`。构建后通过 `objcopy -O binary` 转换成扁平二进制——和生产内核完全相同的后处理流程。最终产物是 `build/kernel/mini/mini_kernel_test.bin`，它会被打包成测试磁盘映像 `cinux_test.img`，由 QEMU 加载执行。

### 内核端测试入口——`main_test.cpp`

```cpp
#include "../../boot/boot_info.h"
#include "../lib/kprintf.h"

using cinux::mini::lib::kprintf;
using cinux::mini::lib::kdebugf;

extern "C" {
extern uint64_t __boot_info_ptr;
void run_cpp_tests();
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;
    (void)boot_info;

    kprintf("=== kprintf Test ===\n");
    kprintf("String: %s\n", "Hello, Cinux!");
    kprintf("Char: %c\n", 'X');
    kprintf("Decimal: %d\n", -12345);
    kprintf("Unsigned: %u\n", 42);
    kprintf("Hex lower: %x\n", 0xDEADBEEF);
    kprintf("Hex upper: %X\n", 0xDEADBEEF);
    kprintf("Pointer: %p\n", 0xFFFFFFFF80000000ULL);
    kprintf("Binary: %b\n", 0b101010);
    kprintf("Width test: [%4d]\n", 7);
    kprintf("Zero pad: [%04d]\n", 7);
    kprintf("Null string: %s\n", nullptr);
    kprintf("Percent: %%\n");

    kdebugf("=== kdebugf Test ===\n");
    kdebugf("Value: %d, Hex: %x\n", -42, 0xDEADBEEF);
    kdebugf("String: %s, Pointer: %p\n", "Debug", 0xFFFFFFFF80000000ULL);

    run_cpp_tests();

    kprintf("\n=== All tests completed ===\n");

    __asm__ volatile("outl %0, $0xf4" : : "a"(0));

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

测试入口函数 `mini_kernel_main` 首先对 kprintf 和 kdebugf 做了一轮格式化冒烟测试——覆盖了所有支持的格式符（`%s`、`%c`、`%d`、`%u`、`%x`、`%X`、`%p`、`%b`、`%%`）以及宽度修饰符（`%4d`、`%04d`）和边界情况（`nullptr` 字符串）。这不是自动化断言测试——这些调用的输出需要人眼检查或者未来的回归测试框架来比对——但它能验证 kprintf 的基本功能在内核环境中确实工作。

最后的 `__asm__ volatile("outl %0, $0xf4" : : "a"(0))` 是测试自动化的关键。QEMU 配置了 `isa-debug-exit` 设备（`-device isa-debug-exit,iobase=0xf4,iosize=0x04`），向端口 0xF4 写入一个双字值会让 QEMU 以特定的退出码退出。退出码的计算公式是 `(value << 1) | 1`，所以写入 0 的退出码是 `(0 << 1) | 1 = 1`。外层的 `run_all_tests.sh` 脚本检查 QEMU 的退出码：如果是 1 就表示测试通过，其他值表示失败。

### C++ 运行时测试——`test_cpp_basic.cpp`

```cpp
#include "../lib/kprintf.h"

using cinux::mini::lib::kprintf;

namespace test {
    static int tests_passed = 0;
    static int tests_failed = 0;
}
```

内核测试框架用了一个极简方案：命名空间 `test` 里的两个静态计数器加上一组宏。和 Host 端的自研框架（`TEST()` 宏 + 自动注册）相比，这里的手动方案更简单——不需要自动注册机制（因为全局构造函数可能在某些测试场景下本身就不可靠），直接在 `main_test.cpp` 里手动调用每个测试函数。

```cpp
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kprintf("[FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            test::tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b) TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NE(a, b) TEST_ASSERT((a) != (b))
#define TEST_ASSERT_NULL(ptr) TEST_ASSERT((ptr) == nullptr)
#define TEST_ASSERT_NOT_NULL(ptr) TEST_ASSERT((ptr) != nullptr)

#define RUN_TEST(fn) \
    do { \
        kprintf("[RUN] %s\n", #fn); \
        fn(); \
        if (test::tests_failed == 0) { \
            test::tests_passed++; \
            kprintf("[PASS] %s\n", #fn); \
        } \
    } while(0)
```

`TEST_ASSERT` 在条件不满足时输出失败信息（包括条件表达式、文件名、行号），增加失败计数器，然后 `return` 退出当前测试函数。`RUN_TEST` 在运行测试函数前后输出标记：`[RUN]` 表示开始，`[PASS]` 表示通过（只有 `tests_failed` 没有增加才算通过）。这种"先跑函数再检查计数器"的模式意味着一个测试函数中如果第一个断言失败，后续断言不会执行——这是一种 fail-fast 策略，避免级联错误。

`RUN_TEST` 有一个微妙的 bug 特征：它在调用 `fn()` 之后检查的是全局的 `tests_failed` 计数器，而不是 `tests_failed` 在 `fn()` 执行前的值。这意味着如果测试 A 失败（`tests_failed` 变成 1），即使测试 B 全部通过，`RUN_TEST(test_B)` 也不会输出 `[PASS]`——因为 `tests_failed` 还是 1。这是一个已知的简化：当前实现不重置 `tests_failed`，所以一旦有测试失败，后续所有测试都会被标记为"跳过 PASS"。这在开发阶段问题不大——你需要修好失败的测试再往下走。

四个测试用例分别验证 C++ 运行时的不同方面。

**Test 1: 简单类构造/析构**

```cpp
namespace test1 {
    static int constructor_calls = 0;
    static int destructor_calls = 0;

    class SimpleClass {
    private:
        int value;
        char marker;
    public:
        SimpleClass(int v) : value(v), marker('S') {
            constructor_calls++;
        }
        ~SimpleClass() {
            destructor_calls++;
        }
        int getValue() const { return value; }
        char getMarker() const { return marker; }
    };

    void test_simple_class() {
        test1::constructor_calls = 0;
        test1::destructor_calls = 0;

        {
            SimpleClass obj(1);
            TEST_ASSERT_EQ(obj.getValue(), 1);
            TEST_ASSERT_EQ(obj.getMarker(), 'S');
            TEST_ASSERT_EQ(constructor_calls, 1);
            TEST_ASSERT_EQ(destructor_calls, 0);
        }

        TEST_ASSERT_EQ(constructor_calls, 1);
        TEST_ASSERT_EQ(destructor_calls, 1);
    }
}
```

这个测试验证最基本的 C++ 特性：构造函数在对象创建时调用，析构函数在作用域结束时调用。对象在花括号块内创建，出了花括号块就应该被析构。如果 C++ 运行时的 `__cxa_atexit` 注册机制不正常（或者根本没实现），析构函数就不会被调用。

**Test 2: 虚函数**

```cpp
namespace test2 {
    class Base {
    public:
        virtual char getName() = 0;
        virtual int compute() = 0;
        virtual ~Base() {}
    };

    class Derived : public Base {
    private:
        int multiplier;
    public:
        Derived(int m) : multiplier(m) {}
        virtual char getName() override { return 'D'; }
        virtual int compute() override { return multiplier * 2; }
        virtual ~Derived() override {}
    };

    void test_virtual_functions() {
        Derived derived(5);
        Base* base = &derived;

        TEST_ASSERT_EQ(base->getName(), 'D');
        TEST_ASSERT_EQ(base->compute(), 10);
    }
}
```

虚函数测试验证的是 vtable（虚函数表）机制是否正常工作。`Base* base = &derived` 是一个向上转型（upcast），`base->getName()` 通过 vtable 分派到 `Derived::getName()`。如果 vtable 没有被正确初始化（比如 RTTI 被禁用但编译器还是生成了 vtable 引用，或者 `.rodata` 段的布局有问题），这里会调用纯虚函数或者跳转到错误地址。

**Test 3: 全局对象构造**

```cpp
namespace test3 {
    static int global_construction_count = 0;

    class GlobalCounter {
    public:
        GlobalCounter() {
            global_construction_count = 42;
        }
        int getCount() const { return global_construction_count; }
    };

    static GlobalCounter global_counter;

    void test_global_construction() {
        TEST_ASSERT_EQ(global_counter.getCount(), 42);
    }
}
```

这个测试验证 `.init_array` 机制是否正常：`global_counter` 是一个 `static` 全局对象，它的构造函数应该在 `mini_kernel_main` 之前被 `_init_global_ctors()` 调用。如果链接脚本的 LMA 计算有误（上一篇文章讲的那个 `SIZEOF()` bug），`.init_array` 中的函数指针就是垃圾值，`global_counter` 的构造函数根本不会被调用，`global_construction_count` 保持为 0，测试失败。这就是为什么 LMA bug 的影响如此深远——它不仅影响 `g_serial` 的初始化，还影响所有全局对象。

**Test 4: 多重继承**

```cpp
namespace test4 {
    class Base1 {
    public:
        virtual int f1() { return 1; }
        virtual ~Base1() = default;
    };

    class Base2 {
    public:
        virtual int f2() { return 2; }
        virtual ~Base2() = default;
    };

    class Multi : public Base1, public Base2 {
    public:
        virtual int f1() override { return 11; }
        virtual int f2() override { return 22; }
    };

    void test_multiple_inheritance() {
        Multi m;
        Base1* b1 = &m;
        Base2* b2 = &m;

        TEST_ASSERT_EQ(b1->f1(), 11);
        TEST_ASSERT_EQ(b2->f2(), 22);
    }
}
```

多重继承的 vtable 布局比单继承复杂得多。`Multi` 对象有两个 vtable 指针（一个从 `Base1` 继承，一个从 `Base2` 继承），`Base2* b2 = &m` 实际上需要 `this` 指针调整（`b2` 指向 `m` 内部的 `Base2` 子对象，而不是 `m` 的起始地址）。如果编译器的 `-fno-rtti` 标志影响了 thunk 生成（虽然通常不会），或者 thunk 代码的地址映射有问题，这个测试就会失败。在 Cinux 的环境中，这个测试能通过说明内核的代码段和数据段映射都是正确的。

### Host 端格式化测试——`test/unit/test_kprintf_format.cpp`

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

Host 端测试通过 `#define TEST_FRAMEWORK_IMPL` 引入自研测试框架的实现（不是只引入声明），然后通过 `#include "mini/lib/private/format.h"` 直接引用内核的格式化函数。注意这里没有 `#include "mini/lib/kprintf.h"`——测试只关注底层的格式化算法，不涉及串口输出。

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

这里展示的几个测试用例覆盖了关键边界情况：`INT64_MIN`（负数绝对值溢出）、`0x123456789ABCDEF0`（完整 16 位十六进制，包含所有 0-9 和 a-f 字符）、`0xFFFFFFFFFFFFFFFF`（64 位全 1）。Host 端测试的优势在于可以直接用 `std::string` 做字符串比较——这在内核端是做不到的，因为 `std::string` 需要堆分配。

Host 测试能直接测试内核源文件（`format.cpp`），是因为 `format.cpp` 的代码是纯算法——不依赖串口、不依赖 I/O 端口、不依赖任何内核特有的基础设施。只要用 `-DCINUX_HOST_TEST` 编译，`format.cpp` 就能在 Linux 用户态正常编译和运行。这种"算法代码在 Host 和 Kernel 之间复用"的模式是 Cinux 测试架构的设计亮点。

### Host 测试构建——`test/CMakeLists.txt`

```cmake
set(CINUX_HOST_TEST ON)

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

enable_testing()
```

`enable_testing()` 启用 CMake 的 CTest 功能。`TEST_INCLUDE_DIRS` 把测试框架头文件和内核源文件目录都加到 include 路径中——前者是为了 `#include "test_framework.h"`，后者是为了 `#include "mini/lib/private/format.h"`。

```cmake
add_executable(test_kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)

target_compile_definitions(test_kprintf_format PRIVATE CINUX_HOST_TEST)
target_include_directories(test_kprintf_format PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME kprintf_format COMMAND test_kprintf_format)
set_tests_properties(kprintf_format PROPERTIES LABELS "kprintf_format")
```

`test_kprintf_format` 的源文件列表混合了测试代码和内核实现代码——`unit/test_kprintf_format.cpp`（测试端）和 `kernel/mini/lib/private/format.cpp`（被测端）。`CINUX_HOST_TEST` 宏让测试代码中的 `#ifdef CINUX_HOST_TEST` 分支生效，启用 `printf` 输出和标准库引用。

```cmake
configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)

add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)
```

`configure_file` 用 `@ONLY` 模式处理 `run_all_tests.sh.in` 模板——把 `@CMAKE_CTEST_COMMAND@`、`@QEMU_EXECUTABLE@` 等占位符替换成实际的值。`add_custom_target(test)` 定义了统一的 `make test` 目标，依赖 `test_smoke`（冒烟测试）、`test_kprintf_format`（格式化测试）和 `test-image`（测试磁盘映像）。运行顺序由脚本控制：先跑 Host 端 CTest，再跑 QEMU 内核测试。

### 测试运行脚本——`scripts/run_all_tests.sh.in`

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

脚本的逻辑很清晰。`set -e` 确保任何命令失败时立即退出——Host 测试如果失败，就不会启动 QEMU 跑内核测试。Host 测试通过后，`set +e` 临时关闭退出检查，因为 QEMU 的退出码需要手动判断。`isa-debug-exit` 设备的退出码公式是 `(value << 1) | 1`：内核写入 0，QEMU 退出码为 1，表示测试通过。如果内核中途崩溃（Page Fault、Triple Fault 等），QEMU 的退出码会是其他值——脚本据此判断测试是否成功。

`@QEMU_COMMON_FLAGS_STR@` 和 `@QEMU_TEST_EXTRA_FLAGS_STR@` 是 CMake 在 `configure_file` 时替换的。`QEMU_TEST_EXTRA_FLAGS` 包含 `-device isa-debug-exit,iobase=0xf4,iosize=0x04`，这是内核端测试自动退出的硬件基础。

### 用户端测试启动脚本——`scripts/run_all_test_user_scripts.sh`

```bash
#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

source "${SCRIPT_DIR}/log/logging.sh"

log_info "========================================"
log_info "Cinux OS - QEMU 调试模式启动"
log_info "========================================"

CLEAN_CMD="rm -rf \"$BUILD_DIR\""
log_cmd "${CLEAN_CMD}"
eval "${CLEAN_CMD}"

log_info ""
log_info "[1/2] 配置 CMake (Debug 模式)..."
CMAKE_CMD="cmake -B \"$BUILD_DIR\" -DCINUX_BUILD_TESTS=ON -S \"$PROJECT_ROOT\""
log_cmd "$CMAKE_CMD"
eval "$CMAKE_CMD"

log_info ""
log_info "[2/2] 启动所有的测试脚本...."
BUILD_CMD="cmake --build \"$BUILD_DIR\" --target test -j\"$(nproc)\" "
log_cmd "$BUILD_CMD"
eval "$BUILD_CMD"
```

这是一个面向用户的端到端测试脚本：清空 build 目录、重新配置 CMake（启用测试）、构建并运行全部测试。它通过 `source "${SCRIPT_DIR}/log/logging.sh"` 引入项目的日志工具函数，用 `log_info` 和 `log_cmd` 输出带格式的日志信息。完整的流程是从干净状态开始构建，确保测试结果不受旧构建产物的干扰。

### QEMU 调试/测试目标——`cmake/qemu.cmake`

005 在 `qemu.cmake` 中新增了测试相关的 QEMU 目标和标志：

```cmake
set(QEMU_DEVELOP_FLAG
    -no-shutdown)

set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)

string(REPLACE ";" " " QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS}")
string(REPLACE ";" " " QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS}")
set(QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS_STR}" CACHE INTERNAL "")
set(QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS_STR}" CACHE INTERNAL "")
```

`-no-shutdown` 从 `QEMU_COMMON_FLAGS` 移到了 `QEMU_DEVELOP_FLAG`，这意味着正常开发模式下 QEMU 遇到 triple fault 会保持不退出（方便调试），而测试模式下没有这个标志（方便自动退出）。`isa-debug-exit` 设备只在测试相关的目标中使用，不影响正常运行的 QEMU 实例。

CMake list 到字符串的转换（`string(REPLACE ";" " " ...)`)是为了在 `configure_file` 生成 shell 脚本时能正确嵌入 QEMU 参数——CMake 的 list 在内部用分号分隔，直接塞到 shell 命令里会变成一坨带分号的字符串。

```cmake
add_custom_target(run-kernel-test
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (auto-exit)"
    VERBATIM
)
```

`run-kernel-test` 目标加载测试磁盘映像（`cinux_test.img`）并启用 `isa-debug-exit`。`USES_TERMINAL` 确保 QEMU 能直接访问当前终端（串口输出 `-serial stdio` 需要这个）。`DEPENDS test-image` 保证先构建测试映像再运行。

### 根目录构建配置变更——`CMakeLists.txt`

```cmake
add_subdirectory(boot)
add_subdirectory(kernel)

include(cmake/qemu.cmake)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()
```

005 做了一个看起来很小但很关键的改动：`include(cmake/qemu.cmake)` 从 `if(CINUX_BUILD_TESTS)` 之后移到了之前。原因是 `cmake/qemu.cmake` 中定义了 `run-kernel-test` 等测试目标，这些目标在 `test/CMakeLists.txt` 的 `add_custom_target(test)` 中被引用为依赖项。如果 `cmake/qemu.cmake` 在测试子目录之后才被 include，那些目标就还没定义——CMake 会报错。调整顺序后，QEMU 相关目标先定义，然后测试子目录可以正确引用它们。

### 内核构建配置——`kernel/mini/CMakeLists.txt`

```cmake
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

add_executable(mini_kernel
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    main.cpp
    driver/serial.cpp
    lib/kprintf.cpp
    lib/private/format.cpp
)

target_compile_options(mini_kernel PRIVATE ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})

target_include_directories(mini_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_options(mini_kernel PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel>
            $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)

add_subdirectory(lib)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()
```

`MINI_KERNEL_COMMON_COMPILE_OPTIONS` 和 `MINI_KERNEL_COMMON_LINK_OPTIONS` 是 005 新增的变量——它们被提取为公共变量，供生产内核和测试内核共享。在此之前，编译选项是直接写在 `target_compile_options` 里的，测试内核如果要保持一致的编译选项就得复制一遍。提取成变量后，任何编译选项的修改只需要改一处。

源文件列表新增了 `driver/serial.cpp`、`lib/kprintf.cpp`、`lib/private/format.cpp`——这三个文件构成了 tag 005 的核心功能代码。`lib/` 子目录通过 `add_subdirectory(lib)` 加入构建（它编译了 `kprintf_private` 静态库，主要供 Host 端测试使用）。测试子目录通过 `CINUX_BUILD_TESTS` 条件控制——默认不构建测试，需要显式传 `-DCINUX_BUILD_TESTS=ON`。

## 设计决策

### 决策：双模式测试 vs 纯内核测试

**问题**：内核代码的测试策略是什么？所有测试都在 QEMU 里跑，还是分 Host/Kernel 两种模式？

**本项目的做法**：双模式。纯算法代码（格式化函数）在 Host 端用 CTest 测试，涉及硬件和运行时的代码（串口驱动、C++ 构造/析构）在 QEMU 内核模式测试。

**备选方案**：全部在 QEMU 里跑。写一个统一的测试内核，包含所有测试用例。

**为什么选双模式**：Host 端测试有一个压倒性的优势——快。`make test_host` 在 100 毫秒内就能跑完，不需要启动 QEMU、不需要等 bootloader 执行、不需要等内核初始化。在 TDD（测试驱动开发）的循环中，"写代码 → 跑测试 → 看结果 → 改代码"的反馈速度直接决定了开发效率。而且 Host 端测试可以用 GDB 单步调试、用 Valgrind 检查内存、用 `std::string` 做断言比较——这些工具在 QEMU 环境下要么不可用，要么使用成本极高。

但有些代码只能在内核环境测试：全局构造函数依赖 `.init_array` 段和 bootloader 的初始化流程，虚函数依赖 vtable 在高半核地址空间中的正确映射，串口发送依赖 I/O 端口权限（Ring 0）。这些只能在 QEMU 里验证。双模式架构让每种测试在最合适的环境中运行。

### 决策：自研测试框架 vs Google Test

**问题**：Host 端测试用什么框架？

**本项目的做法**：自研轻量框架，位于 `test/framework/test_framework.h`。核心是 `TEST()` 宏（自动注册）和 `ASSERT_*` 宏。

**备选方案**：引入 Google Test（`gtest`）。

**为什么选自研**：Google Test 功能强大但很重——需要 CMake 的 `FetchContent` 或系统安装、链接额外的库、处理 `main()` 函数冲突。对于一个教学 OS 项目来说，测试框架本身的复杂度不应该超过被测代码。自研框架总共不到 200 行，支持自动注册、断言、通过/失败统计——足够了。而且自研框架不依赖任何标准库功能（输出通过宏抽象），可以轻松适配到内核环境。

**扩展方向**：如果测试规模增长到需要参数化测试和 fixture，可以考虑切到 Google Test。当前的 `TEST()` 宏在概念上和 gtest 的 `TEST()` 完全兼容，迁移成本很低。

### 决策：`isa-debug-exit` 自动退出 vs 手动 Ctrl+C

**问题**：内核测试在 QEMU 里跑完后怎么退出？

**本项目的做法**：QEMU 的 `isa-debug-exit` 设备 + 内核端 `outl $0xf4` 指令。

**备选方案**：QEMU 的 `-device shutdown` 监控命令、或者 QEMU monitor 的 `quit` 命令、或者超时后 `kill` 进程。

**为什么选 isa-debug-exit**：它是专门为测试设计的退出机制——简单、可靠、无歧义。内核向端口 0xF4 写入一个值，QEMU 立即以确定的退出码退出。退出码 `(value << 1) | 1` 的计算方式意味着你可以通过写入不同的值来区分不同的退出原因（比如测试通过写 0，测试失败写 1）。外层脚本根据退出码判断测试结果，完全自动化。相比"超时后 kill"，这种方式不需要猜测"内核到底跑完了还是卡住了"。

## 扩展方向

1. **添加 GDB 自动化测试**（难度：困难）——当前测试只能验证"内核跑到某个点后正确退出"。通过 GDB 的 Python 脚本接口，可以在测试运行时自动检查寄存器值、内存内容、调用栈深度等。比如"验证 `__boot_info_ptr` 的值等于 0x7000"、"验证 `g_serial` 的 `base_port` 等于 0x3F8"。

2. **回归测试基准**（难度：中等）——当前 kprintf 的格式化冒烟测试（`main_test.cpp` 中的那些 `kprintf` 调用）需要人眼看输出。可以把预期输出保存为基准文件，测试运行后自动比对实际输出和预期输出。这需要在 QEMU 的 `-serial` 参数中使用 `file:` 模式把串口输出重定向到文件。

3. **CI/CD 集成**（难度：简单）——当前测试基础设施已经支持 CI 集成。只需要写一个 `.github/workflows/test.yml`：安装 `qemu-system-x86_64` 和交叉编译工具链，运行 `cmake -DCINUX_BUILD_TESTS=ON -B build && cmake --build build --target test`。完整的测试在 CI 环境中应该能在 1-2 分钟内完成。

4. **覆盖率收集**（难度：中等）——Host 端测试可以用 `gcov`/`lcov` 收集代码覆盖率。在 `test/CMakeLists.txt` 中加入 `-fprofile-arcs -ftest-coverage` 编译选项和 `-lgcov` 链接选项，运行测试后用 `lcov --capture` 生成覆盖率报告。内核端覆盖率收集更复杂（需要在 QEMU 中使用 GDB 插件或者手动插桩），可以后续考虑。

5. **性能基准测试**（难度：中等）——在内核端添加基于 TSC（Time Stamp Counter）的性能测试：`rdtsc` 读取开始时间，执行被测操作，`rdtsc` 读取结束时间，通过 kprintf 输出耗时。可以测试串口吞吐量、kprintf 格式化速度、内存操作延迟等。

## 参考资料

- QEMU Documentation — isa-debug-exit (https://qemu.readthedocs.io/en/v9.1.3/system/gdb.html)：QEMU 内置调试设备文档，`isa-debug-exit` 的 I/O 端口配置和退出码计算公式。

- OSDev Wiki — GDB (https://wiki.osdev.org/GDB)：OS 开发环境下的 GDB 远程调试指南，包括 QEMU `-s -S` 参数的使用和 VSCode 集成。

- CMake Documentation — CTest (https://cmake.org/cmake/help/latest/manual/ctest.1.html)：CTest 的命令行参数和配置选项，`--output-on-failure`、`-R`（按名称过滤）、`-L`（按标签过滤）等。

- OSDev Wiki — Calling Global Constructors (https://wiki.osdev.org/Calling_Global_Constructors)：`.init_array` 段的布局和遍历方式。Cinux 的内核测试（test3: 全局对象构造）直接验证了这一机制在高半核地址空间中的正确性。
