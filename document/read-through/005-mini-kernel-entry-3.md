---
title: 005-mini-kernel-entry-3 · 内核入口
---

# 005 通读版 · 测试框架与调试工具链——内核也能做 TDD

## 概览

前两篇文章覆盖了串口驱动和 kprintf 格式化库——它们是内核的"嘴巴"。但光能说话还不够，我们还需要一套机制来验证内核代码是否正确工作。在用户态开发中，你可以用 Google Test、Catch2、甚至简单的 `assert()` 来验证逻辑。但在内核环境下，事情要复杂得多：标准库不可用、异常禁用、没有 `main()` 函数的标准入口、程序运行在 QEMU 模拟的裸机环境中。怎么在这样的条件下写测试？

Cinux 的答案是双模式测试架构：Host 端测试在 Linux 用户态直接运行，验证纯算法逻辑（比如格式化函数）；内核端测试编译为独立的测试内核，通过 QEMU 启动后在真实的内核环境中运行，验证 C++ 运行时特性（构造/析构、虚函数、多重继承）。两种模式通过 CMake/CTest 统一调度，一条 `make test` 命令跑完全部测试。内核端测试还能通过 QEMU 的 `isa-debug-exit` 设备自动退出，实现完全自动化的测试流程。

本文覆盖内核端测试构建配置（`kernel/mini/test/CMakeLists.txt`）和入口（`main_test.cpp`）、C++ 运行时测试用例（`test_cpp_basic.cpp`）。Host 端测试、测试自动化脚本和构建配置在下一篇中详细讲解。

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

## 参考资料

- OSDev Wiki — Calling Global Constructors (https://wiki.osdev.org/Calling_Global_Constructors)：`.init_array` 段的布局和遍历方式。Cinux 的内核测试（test3: 全局对象构造）直接验证了这一机制在高半核地址空间中的正确性。
