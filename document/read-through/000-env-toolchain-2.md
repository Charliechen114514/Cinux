# 000-2 · MBR 存根与测试框架

## 概览

本文是 tag `000_env_toolchain` 的第二篇通读，聚焦两个子系统：MBR 存根（`boot/mbr_stub.S`）和自研测试框架（`test/framework/test_framework.h` + `test/unit/test_smoke.cpp`）。MBR 存根是整个 Cinux 启动链的第一个环节——虽然它只做"清屏、打字、停机"三件事，但它验证了从汇编到 QEMU 启动的完整流水线。测试框架则是贯穿后续所有 tag 的基础设施，有了它我们才能在 Host 端和 QEMU 端同时跑单元测试。这两个模块在功能上没有直接关联，但它们共享同一个目标：为后续内核开发搭建可验证的基础。

## 架构图

```
                    QEMU 启动流程
                    ┌─────────┐
                    │  BIOS   │ POST → 加载第一扇区
                    └────┬────┘
                         │ 加载到 0x7C00
                         ▼
              ┌─────────────────────┐
              │   boot/mbr_stub.S   │
              │                     │
              │  1. 清屏 (VGA)      │
              │  2. 打印 'C'        │
              │  3. cli + hlt       │
              │  4. .fill + 0xAA55  │
              └─────────────────────┘

                    测试框架架构
              ┌─────────────────────┐
              │  test_framework.h   │
              │                     │
              │  TEST("name") { }   │──→ 自动注册到 _test_registry[]
              │  ASSERT_*(...)      │──→ 失败时 return
              │  RUN_ALL_TESTS()    │──→ 遍历执行 + 统计
              └──────────┬──────────┘
                         │ 条件编译
                ┌────────┴────────┐
                ▼                 ▼
         Host 模式            QEMU 模式
      -DCINUX_HOST_TEST     内核内运行
         printf()          serial_printf()
```

## 代码精讲

### MBR 存根 — 文件头与模式声明

```asm
.section .text
.code16
.global _start
```

`.section .text` 把后续内容放入代码段。`.code16` 是最关键的一条——它告诉 GNU AS 汇编器"请生成 16 位指令"，因为我们这段代码会被 BIOS 在实模式下执行，CPU 此时还处于 16 位状态，如果生成 32 位或 64 位指令，CPU 根本无法识别，直接 triple fault。`.global _start` 让 `_start` 标签对外可见，这样链接器才能把它设为入口点——对应的链接脚本里有 `ENTRY(_start)` 与之呼应。

### 清屏：用 rep stosw 暴力清空 VGA 显存

```asm
_start:
    mov $0xB800, %ax
    mov %ax, %es
    xor %di, %di
    mov $0x0720, %ax
    mov $2000, %cx
    cld
    rep stosw
```

这段代码在做的事情是：把 VGA 文本模式显存里的 2000 个字符单元全部填成"灰色背景的空格"。我们逐行来看。

`mov $0xB800, %ax` 然后 `mov %ax, %es` 这两条把 ES 段寄存器设为 `0xB800`。在实模式下，内存地址是 `段 × 16 + 偏移` 的形式，所以 ES=`0xB800` 对应的物理地址就是 `0xB800 × 16 = 0xB8000`——正好是 VGA 文本缓冲区的起始地址。之所以不直接 `mov $0xB800, %es`，是因为 x86 不允许立即数直接加载到段寄存器，必须通过通用寄存器中转。

`xor %di, %di` 把 DI 清零，这样 ES:DI 就精确指向 `0xB8000:0x0000`，也就是屏幕左上角第一个字符。

`mov $0x0720, %ax` 加载的值 `0x0720` 是两个字节：低字节 `0x20` 是空格字符的 ASCII 码，高字节 `0x07` 是显示属性（灰底黑字，具体来说 bit3-0=0111 是浅灰色前景，bit6-4=000 是黑色背景）。`rep stosw` 指令每次把 AX（2 字节）写入 ES:DI，然后 DI 自动加 2，重复 CX 次——所以 CX 设为 2000（80列 × 25行），执行完毕后整个屏幕就被清成灰色空格了。`cld` 清除方向标志确保 DI 是递增而不是递减，这一步不能省，因为 BIOS 传给我们的初始 EFLAGS 状态里 DF 位的值是未定义的。

### 打印字符 'C'

```asm
    mov $0x1F, %ah
    mov $'C', %al
    mov %ax, %es:(0)
```

属性字节 `0x1F` 的含义是：bit6-4 = 001（蓝色背景），bit3-0 = 1111（亮白色前景）——合起来就是蓝底白字。字符 'C' 的 ASCII 码加载到 AL，然后 AX 整体（属性在高字节，字符在低字节）写入 ES:0，也就是屏幕坐标 (0,0)。这一步执行后，QEMU 窗口左上角就会出现一个醒目的蓝底白字 C。

这里你会发现一个设计细节：我们是直接写 VGA 显存而不是调 BIOS 中断（比如 `INT $0x10`）。直接写显存更快也更直接，而且不受 BIOS 中断实现的限制。当然，这要求我们自己去管理光标位置和屏幕滚动，不过在 tag 000 里我们只写一个字符，不需要操心这些。

### 停机与无限循环

```asm
    cli
    hlt
    jmp _start
```

`cli` 清除 EFLAGS 的 IF 位，禁用所有可屏蔽中断。`hlt` 让 CPU 进入停机状态——此时 CPU 不会执行任何指令，直到收到一个中断信号。但因为前面已经 `cli` 关中断了，正常情况下不会有中断来唤醒 CPU。万一有什么不可屏蔽中断（NMI）或者其他意外情况唤醒了 CPU，`jmp _start` 会让它跳回去重新执行 `cli; hlt`，确保永远不会再往下执行到未初始化的内存区域。

### 填充与签名

```asm
.fill 510 - (. - _start), 1, 0
.word 0xAA55
```

`.fill` 是 GNU AS 的填充指令，三个参数分别是：填充字节数、每个字节的值、填充值（这里是 0）。计算方式 `510 - (. - _start)` 中，`.` 代表当前位置，`_start` 是代码起始位置，所以 `(. - _start)` 就是代码部分占了多少字节，用 510 减去它就是还需要填充多少零才能到达偏移 510。加上最后的 2 字节签名，总长度恰好 512 字节——一个完整的扇区。

`.word 0xAA55` 写入的值在小端序下实际存储为字节序列 `0x55, 0xAA`。这个签名是 BIOS 检测有效 MBR 的唯一判据——如果扇区最后两个字节不是 `0x55 0xAA`（文件中的顺序），BIOS 会拒绝执行这个扇区并报 "No bootable device" 之类的错误。

### 测试框架 — 平台适配层

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

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

测试框架通过 `CINUX_HOST_TEST` 宏实现双模式运行。Host 模式下，测试代码用系统 g++ 编译，有标准库可用，所以 `printf` 直接输出到终端，`abort()` 在失败时终止进程。QEMU 模式下，测试代码跑在内核里，没有标准库，输出依赖内核的 `serial_printf`（通过串口发送字符），失败时执行 `hlt` 指令让 CPU 停机。两种模式共享同一套 `TEST` 和 `ASSERT` 宏接口，只是底层的打印和终止实现不同。这种设计的好处是：同一个测试既能在开发机上快速跑一遍验证逻辑正确性，也能在 QEMU 里跑一遍验证真实硬件环境下的行为。

### 测试注册机制

```cpp
typedef void (*_test_fn_t)(void);

struct _TestEntry {
    const char* name;
    const char* file;
    int         line;
    _test_fn_t  fn;
};

#define _MAX_TESTS 256

#ifdef CINUX_HOST_TEST
extern _TestEntry _test_registry[_MAX_TESTS];
extern int        _test_count;

static inline void _register_test(const char* name, const char* file, int line, _test_fn_t fn) {
    if (_test_count < _MAX_TESTS) {
        _test_registry[_test_count++] = {name, file, line, fn};
    }
}
#endif
```

测试条目用一个简单的结构体 `_TestEntry` 描述——包含名字、文件名、行号和函数指针。Host 模式下使用静态数组 `_test_registry[]` 存储（最多 256 个），`_register_test` 函数往数组末尾追加。为什么用静态数组而不是链表或者动态分配？因为测试框架本身就是基础设施，不能依赖任何可能还没实现的功能（比如 `kmalloc`），静态数组是最简单、最可靠的选择。

### TEST 宏与自动注册

```cpp
#define _TEST_CAT2(a, b) a##b
#define _TEST_CAT(a, b)  _TEST_CAT2(a, b)

#define TEST(test_name)                                                                            \
    static void _TEST_CAT(_test_fn_, __LINE__)();                                                  \
    static struct _TEST_CAT(_TestAutoReg_, __LINE__) {                                             \
        _TEST_CAT(_TestAutoReg_, __LINE__)() {                                                     \
            _register_test(test_name, __FILE__, __LINE__, _TEST_CAT(_test_fn_, __LINE__));         \
        }                                                                                          \
    } _TEST_CAT(_test_reg_, __LINE__);                                                             \
    static void _TEST_CAT(_test_fn_, __LINE__)()
```

这里是整个框架最精巧的部分。`TEST("name") { ... }` 宏展开后做了三件事：

第一，声明一个静态函数 `_test_fn_XXX`（XXX 是行号），它的函数体就是用户写的 `{ ... }` 部分。第二，定义一个静态结构体变量 `_TestAutoReg_XXX`，这个结构体的构造函数里调用 `_register_test` 把测试函数注册到全局数组里。第三，由于这个结构体变量是静态全局变量，它的构造函数会在 `main` 函数之前被自动执行——这就是"自动注册"的实现原理，用户不需要手动维护一个测试列表。

`_TEST_CAT` 和 `_TEST_CAT2` 这两层宏是 C 预处理的一个经典技巧。`##` 运算符会把两个 token 粘在一起，但它不会先展开操作数。如果我们直接写 `_test_fn_##__LINE__`，结果是字面的 `_test_fn___LINE__` 而不是 `_test_fn_42`。解决方法是增加一层间接：`_TEST_CAT(a, b)` 调用 `_TEST_CAT2(a, b)`，在调用 `_TEST_CAT` 的时候 `__LINE__` 已经被展开成了数字（比如 42），然后 `_TEST_CAT2(_test_fn_, 42)` 才真正执行 `##` 拼接，得到 `_test_fn_42`。

### ASSERT 宏

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

`ASSERT_EQ` 的实现用 `do { ... } while(0)` 包裹——这是 C/C++ 宏的标准防御性写法，确保宏在使用时后面能正确加分号，不会在 if-else 语境里出问题。`auto _a = (actual)` 把参数存到局部变量里，避免参数被求值多次（如果 `actual` 是一个有副作用的表达式比如 `i++`，不存的话会比较一次、打印时又求值一次，逻辑就错了）。

失败时的处理是：打印测试名和断言位置（用 `#actual` 把表达式原样字符串化），递增全局失败计数，然后 `return`——注意是 `return` 而不是 `abort`，这样当前测试会提前退出，但不会影响后续测试的执行。`_current_test_name` 是一个全局字符串指针，在 `RUN_ALL_TESTS` 的循环里每次运行新测试前更新。

其他断言宏都是类似的模式：`ASSERT_TRUE` 直接检查表达式，`ASSERT_FALSE` 取反，`ASSERT_NULL` 和 `ASSERT_NOT_NULL` 检查指针，`ASSERT_GE/LE/GT/LT` 做比较——它们最终都委托给 `ASSERT_TRUE`，这样代码量最少，维护也方便。

### RUN_ALL_TESTS

```cpp
extern const char* _current_test_name;

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
}
```

运行逻辑很直接：遍历注册数组，依次调用每个测试函数。每个测试执行前更新 `_current_test_name`（供 ASSERT 宏打印用），执行后直接打印 `[PASS]`。如果一个测试中途断言失败，它内部的 `return` 会把控制流带回循环，但此时 `_tests_failed` 已经被递增了，而 `_tests_passed` 也会被递增——这是一个已知的简化缺陷：per-test 的通过/失败追踪不够精确。不过对于 tag 000 的冒烟测试来说完全够用，后续版本可以改进。

### 全局变量定义

```cpp
#ifdef TEST_FRAMEWORK_IMPL
_TestEntry  _test_registry[_MAX_TESTS];
int         _test_count        = 0;
int         _tests_passed      = 0;
int         _tests_failed      = 0;
const char* _current_test_name = "";
#endif
```

头文件里声明了 `extern` 全局变量，但定义只能出现在一个翻译单元里。`TEST_FRAMEWORK_IMPL` 宏就是用来控制这一点的——在 `test_smoke.cpp` 的第一行定义这个宏（`#define TEST_FRAMEWORK_IMPL`），然后 `#include` 测试框架头文件，全局变量就只会被定义一次。这是单头文件库（stb 风格）的标准做法，所有东西都在一个 `.h` 里，通过宏控制哪些是声明、哪些是定义。

### 冒烟测试

```cpp
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

TEST("smoke: 1+1=2") {
    int a        = 1;
    int b        = 1;
    int expected = 2;
    int result = a + b;
    ASSERT_EQ(result, expected);
    ASSERT_TRUE(result == expected);
    ASSERT_FALSE(result != expected);
    ASSERT_GT(result, 1);
    ASSERT_GE(result, 2);
    ASSERT_LE(result, 2);
    ASSERT_LT(result, 3);
}

TEST("smoke: boundary values") {
    int zero = 0;
    ASSERT_EQ(zero, 0);
    ASSERT_TRUE(zero == 0);
    ASSERT_GE(zero, 0);
    ASSERT_LE(zero, 0);

    int negative = -1;
    ASSERT_EQ(negative, -1);
    ASSERT_LT(negative, 0);
}

TEST("smoke: pointer assertions") {
    int* null_ptr = nullptr;
    ASSERT_NULL(null_ptr);
    ASSERT_EQ(null_ptr, nullptr);

    int  value     = 42;
    int* valid_ptr = &value;
    ASSERT_NOT_NULL(valid_ptr);
    ASSERT_EQ(*valid_ptr, 42);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
```

冒烟测试的唯一目的是验证框架本身能正常工作。三个测试用例分别覆盖：整数比较（`ASSERT_EQ/TRUE/FALSE/GT/GE/LE/LT`）、边界值（零和负数）、指针断言（`ASSERT_NULL/NOT_NULL`）。`main` 函数调用 `RUN_ALL_TESTS()` 跑完全部测试后，根据失败数返回 0 或 1——这个返回值会被 CTest 捕获，用来判定测试是通过还是失败。

## 设计决策

### 决策：为什么自研测试框架

**问题**: 内核开发需要单元测试，但 Google Test / Catch2 等标准框架依赖标准库和动态内存分配，在 freestanding 环境下无法使用。

**本项目的做法**: 自研一个极简的单头文件测试框架，只依赖 `printf`（Host 模式）或 `serial_printf`（QEMU 模式），零动态内存分配，零异常依赖。

**备选方案**: 可以使用 Google Test 的 freestanding 裁剪版，或者用 `assert()` + 手写 main 来做最简单的测试。

**为什么不选备选方案**: Google Test 即使裁剪后仍然很重，而且它的构建集成对内核项目来说是个麻烦事。`assert()` 方案则没有测试发现和注册机制，测多了之后管理测试列表非常痛苦。

**如果要扩展**: 可以加 EXPECT 系列宏（不终止当前测试，只记录失败）、测试夹具（setup/teardown）、测试过滤（按名字模式选择运行哪些测试）。

### 决策：MBR 存根为什么只做清屏打字停机

**问题**: 第一个 tag 的 MBR 应该做到什么程度？

**本项目的做法**: 只验证构建链条——汇编→链接→objcopy→dd→QEMU 全流程跑通即可。

**备选方案**: 可以在 tag 000 就做完整的 MBR bootloader（加载 stage2、切换保护模式等）。

**为什么不选备选方案**: 环境搭建和引导逻辑是两个独立的关注点，混在一起会让读者在调试构建系统问题的时候还要同时操心 CPU 模式切换的细节。拆开之后，tag 000 纯粹解决"工具链和构建系统"，tag 001 才开始处理"CPU 到底怎么从实模式启动"。

**如果要扩展**: 可以在 MBR 存根里加一个 BIOS 中断调用来打印字符串（`INT $0x10 AH=0x0E`），验证 BIOS 中断在 QEMU 里能用——这会在 tag 001 实现。

## 扩展方向

- ⭐ 在 MBR 存根里用 `rep stosw` 往整个屏幕填充不同颜色的字符，制造一个彩色条纹效果，加深对 VGA 属性字节的理解
- ⭐ 修改属性字节让 'C' 显示成红底绿字（`0x24`），观察 QEMU 窗口的变化
- ⭐⭐ 给测试框架加一个 `ASSERT_STREQ(str1, str2)` 宏，用 `strcmp` 比较字符串，需要处理空指针的情况
- ⭐⭐ 给 RUN_ALL_TESTS 加 per-test 的通过/失败追踪，修复当前"失败的测试也打印 PASS"的问题
- ⭐⭐⭐ 尝试把测试框架改成 QEMU 模式（定义 QEMU 版的 `serial_printf` stub），让测试在内核环境里通过串口输出

## 参考资料

- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式规范和 BIOS 加载行为
- OSDev Wiki — [Real Mode](https://wiki.osdev.org/Real_Mode): 实模式寻址机制和段寄存器
- Wikipedia — [VGA-compatible text mode](https://en.wikipedia.org/wiki/VGA-compatible_text_mode): VGA 文本缓冲区地址和属性字节格式
- Intel SDM — [Volume 1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Basic Execution Environment，实模式寄存器和内存组织
- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones): freestanding 环境和编译 flag 参考
