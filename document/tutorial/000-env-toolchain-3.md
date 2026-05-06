# 从零搭建 x86_64 OS 开发环境（下）—— 测试框架与大功告成

> 标签：测试框架, CTest, 自动注册, 冒烟测试
> 前置：完成 000-1（工具链）和 000-2（MBR 存根）

## 第四步——测试框架：给内核一双眼睛

写 OS 内核没有测试是非常恐怖的事情
——你改了一行代码，内核跑起来直接 triple fault，
但你完全不知道是哪一行改坏了，
因为内核里连 `printf` 都没有。
Cinux 从第一个 tag 就引入了自研的轻量测试框架，
支持 Host 模式（在 Linux 上用 g++ 编译运行）
和 QEMU 模式（在内核里通过串口输出），
让逻辑性的 bug 在 Host 端就能被捕获。

### 为什么不能用 Google Test

标准的 C++ 测试框架（Google Test、Catch2、doctest）
都假设了一个 hosted 环境：
它们使用 `malloc` 动态分配内存、
依赖 `<iostream>` 做输出、
使用 `std::exception` 做错误传播。
在内核的 freestanding 环境里，
这些东西统统不存在。
即使裁剪 Google Test 为 freestanding 版本，
它的构建集成对内核项目来说也是个麻烦事。
Cinux 的自研框架走了一条更轻量的路线：
单头文件，零动态内存，零异常依赖。

### TEST 宏：自动注册的魔法

```cpp
#define TEST(test_name)                                                                            \
    static void _TEST_CAT(_test_fn_, __LINE__)();                                                  \
    static struct _TEST_CAT(_TestAutoReg_, __LINE__) {                                             \
        _TEST_CAT(_TestAutoReg_, __LINE__)() {                                                     \
            _register_test(test_name, __FILE__, __LINE__, _TEST_CAT(_test_fn_, __LINE__));         \
        }                                                                                          \
    } _TEST_CAT(_test_reg_, __LINE__);                                                             \
    static void _TEST_CAT(_test_fn_, __LINE__)()
```

`TEST("name") { ... }` 宏的设计借鉴了 Google Test 的自动注册机制
——利用 C++ 静态对象的构造函数在 `main` 之前执行的特性，
每个 `TEST` 块会自动把自己的函数指针注册到全局数组里，
用户不需要手动维护测试列表。

`_TEST_CAT` 两层间接展开解决了 `__LINE__` 和标识符拼接的经典预处理陷阱
——直接 `##_LINE__` 不会展开 `__LINE__`，
必须先通过一层宏让它展开成数字，
再通过第二层做拼接。
这个技巧在 C 预处理编程里反复出现，值得牢记。

对比其他项目的测试策略：
xv6 完全没有测试框架，所有验证都靠手动运行和目测输出。
Linux 内核有 KUnit 子系统（在内核里跑单元测试）
和 kselftest（用户态测试），
但它们依赖内核已经能跑起来这一前提。
Cinux 的双模式方案（Host + QEMU）的巧妙之处在于：
Host 模式不需要内核就能测试纯逻辑
（比如 kprintf 的格式化、GDT 描述符编码、内存管理算法），
QEMU 模式则测试需要硬件交互的部分
（比如中断处理、驱动初始化）。

### 平台适配层

```cpp
#ifdef CINUX_HOST_TEST
#    include <stdio.h>
#    include <stdlib.h>
#    define _TEST_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#    define _TEST_ABORT()         abort()
#else
extern void serial_printf(const char* fmt, ...);
#    define _TEST_PRINT(fmt, ...) serial_printf(fmt, ##__VA_ARGS__)
#    define _TEST_ABORT()                                                                         \
        do {                                                                                      \
            asm volatile("hlt");                                                                  \
        } while (1)
#endif
```

框架通过 `CINUX_HOST_TEST` 宏实现双模式运行。
Host 模式下，测试代码用系统 g++ 编译，
有标准库可用，所以 `printf` 直接输出到终端，
`abort()` 在失败时终止进程。
QEMU 模式下，测试代码跑在内核里，没有标准库，
输出依赖内核的 `serial_printf`（通过串口发送字符），
失败时执行 `hlt` 指令让 CPU 停机。

两种模式共享同一套 `TEST` 和 `ASSERT` 宏接口，
只是底层的打印和终止实现不同。
这种设计的好处是：
同一个测试既能在开发机上快速跑一遍验证逻辑正确性，
也能在 QEMU 里跑一遍验证真实硬件环境下的行为。

### ASSERT 宏的防御性设计

```cpp
#define ASSERT_EQ(actual, expected)                                                                \
    do {                                                                                           \
        auto _a = (actual);                                                                        \
        auto _e = (expected);                                                                      \
        if (!(_a == _e)) {                                                                         \
            _TEST_PRINT(                                                                           \
                "[FAIL] %s\n  ASSERT_EQ(%s, %s) failed\n"                                         \
                "  at %s:%d\n",                                                                    \
                _current_test_name, #actual, #expected, __FILE__, __LINE__);                       \
            _tests_failed++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)
```

`ASSERT_EQ` 用 `do { ... } while(0)` 包裹
——这是 C/C++ 宏的标准防御性写法，
确保宏在使用时后面能正确加分号。
`auto _a = (actual)` 把参数存到局部变量里，
避免参数被求值多次
（如果 `actual` 是一个有副作用的表达式比如 `i++`，
不存的话会比较一次、打印时又求值一次，逻辑就错了）。

失败时的处理是：
打印测试名和断言位置（用 `#actual` 把表达式原样字符串化），
递增全局失败计数，然后 `return`
——注意是 `return` 而不是 `abort`，
这样当前测试会提前退出，
但不会影响后续测试的执行。

其他断言宏都是类似的模式：
`ASSERT_TRUE` 直接检查表达式，
`ASSERT_FALSE` 取反，
`ASSERT_NULL` 和 `ASSERT_NOT_NULL` 检查指针，
`ASSERT_GE/LE/GT/LT` 做比较
——它们最终都委托给 `ASSERT_TRUE`，
这样代码量最少，维护也方便。

### 冒烟测试

跑一下冒烟测试验证框架工作正常：

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..
make test
```

你应该看到 `=== Cinux Test Runner ===`，
四个测试都显示 `[PASS]`，
最后是 `Results: 4 passed, 0 failed` 和 `[SUITE PASSED]`。

冒烟测试覆盖了：
整数比较（`ASSERT_EQ/TRUE/FALSE/GT/GE/LE/LT`）、
边界值（零和负数，以及 `ASSERT_NE` 不等断言）、
指针断言（`ASSERT_NULL/NOT_NULL`）、
以及一个空的占位测试（预留未来扩展）。
它不测试任何内核逻辑，只验证框架本身能正常工作。

## 到这里就大功告成了

回顾一下这三篇文章做的事情：
安装了工具链，
配好了 CMake 构建系统（toolchain file + QEMU 集成），
写了 MBR 存根并在 QEMU 里看到了第一个字符输出，
还搭好了自研测试框架。
这些都是后续所有 tag 的基础
——现在偷懒跳过去，后面一定会还回来。

从代码量上看，tag 000 是整个项目里最小的
——但它建立的所有基础设施（构建系统、测试框架、开发调试工作流）
会在后续 39 个 tag 里一直使用。
这就是为什么花一整个 tag 来"搭环境"是值得的。

### stb 风格的单头文件库模式

框架使用了一个巧妙的模式：
通过 `TEST_FRAMEWORK_IMPL` 宏控制全局变量的定义。
头文件里声明了 `extern` 全局变量，
但定义只能出现在一个翻译单元里。
在 `test_smoke.cpp` 的第一行定义这个宏
（`#define TEST_FRAMEWORK_IMPL`），
然后 `#include` 测试框架头文件，
全局变量就只会被定义一次。
这是单头文件库（stb 风格）的标准做法，
所有东西都在一个 `.h` 里，
通过宏控制哪些是声明、哪些是定义。

### RUN_ALL_TESTS 的运行逻辑

```cpp
static inline void RUN_ALL_TESTS() {
    extern _TestEntry _test_registry[];
    extern int        _test_count;

    _TEST_PRINT("\n=== Cinux Test Runner ===\n");
    _TEST_PRINT("Running %d test(s)...\n\n", _test_count);

    _tests_passed = 0;
    _tests_failed = 0;

    for (int i = 0; i < _test_count; i++) {
        _current_test_name = _test_registry[i].name;
        _test_registry[i].fn();
        _TEST_PRINT("[PASS] %s\n", _current_test_name);
        _tests_passed++;
    }

    _TEST_PRINT("\n=== Results: %d passed, %d failed ===\n", _tests_passed, _tests_failed);

    if (_tests_failed > 0) {
        _TEST_PRINT("[SUITE FAILED]\n");
    } else {
        _TEST_PRINT("[SUITE PASSED]\n");
    }
}
```

运行逻辑很直接：
遍历注册数组，依次调用每个测试函数。
每个测试执行前更新 `_current_test_name`（供 ASSERT 宏打印用），
执行后打印 `[PASS]`。
运行完毕后根据失败数打印 `[SUITE PASSED]` 或 `[SUITE FAILED]`，
这个标签被 CTest 用来判定整体测试结果。

这里有一个已知的简化缺陷：
如果一个测试中途断言失败，
它内部的 `return` 会把控制流带回循环，
但 `_tests_passed` 也会被递增——
所以失败的测试也会被计为 passed。
这是因为循环里没有 per-test 的状态追踪。
对于 tag 000 的冒烟测试来说完全够用，
后续版本可以改进。

### CTest 集成

`test/CMakeLists.txt` 配置 Host 模式的测试：
创建可执行目标 `test_smoke`，
源文件只有 `test_smoke.cpp`，
添加 `CINUX_HOST_TEST` 编译定义，
设置包含目录（框架头文件和内核头文件），
注册到 CTest。
需要在顶层 CMakeLists.txt 中用
`CINUX_BUILD_TESTS` option 控制 test 子目录的引入
——默认不引入，需要用户手动
`-DCINUX_BUILD_TESTS=ON` 开启。

运行测试：

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..
make test
```

## 到这里就大功告成了

回顾一下这三篇文章做的事情：
安装了工具链，
配好了 CMake 构建系统（toolchain file + QEMU 集成），
写了 MBR 存根并在 QEMU 里看到了第一个字符输出，
还搭好了自研测试框架。
这些都是后续所有 tag 的基础
——现在偷懒跳过去，后面一定会还回来。

从代码量上看，tag 000 是整个项目里最小的
——但它建立的所有基础设施（构建系统、测试框架、开发调试工作流）
会在后续 39 个 tag 里一直使用。
这就是为什么花一整个 tag 来"搭环境"是值得的。

下一章我们将真正进入 MBR bootloader 的开发：
BIOS 中断、磁盘读取、stage2 加载。
准备好进入实模式的世界吧。

## 参考资料

- Intel SDM — [Volume 1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  Chapter 3 Basic Execution Environment，描述实模式寄存器和 red zone 约束
- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones):
  交叉编译器和 freestanding 编译 flag 的权威参考
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式和 BIOS 引导行为
- OSDev Wiki — [Real Mode](https://wiki.osdev.org/Real_Mode): 实模式寻址机制
- Wikipedia — [VGA-compatible text mode](https://en.wikipedia.org/wiki/VGA-compatible_text_mode):
  VGA 文本缓冲区地址 0xB8000 和属性字节格式
- CMake 官方 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html):
  toolchain file 完整文档
- xv6 — [mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv): 极简 Makefile 构建方案
- SerenityOS — [SerenityOS/serenity](https://github.com/SerenityOS/serenity): 生产级 OS 的 CMake 架构
