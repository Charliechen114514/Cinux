---
title: 000-env-toolchain-3 · 环境搭建
---

# 000-3 · 测试框架与完整构建验证

> **本篇目标**：实现自研的轻量测试框架，
> 编写冒烟测试验证框架工作正常，
> 跑通从源码到测试的完整开发循环。
>
> **完成后的效果**：`make test` 全绿通过冒烟测试，
> 测试框架的 Host 模式和 QEMU 模式双轨可用。
>
> **前置**：完成 000-1（工具链 + CMake）和 000-2（MBR 存根 + QEMU）。

---

## 前言：为什么从第一个 tag 就要有测试

写 OS 内核没有测试是非常恐怖的事情
——你改了一行代码，内核跑起来直接 triple fault，
但你完全不知道是哪一行改坏了，
因为内核里连 `printf` 都没有。
传统 OS 教程几乎不提测试：
xv6 完全没有测试框架，所有验证都靠手动运行和目测输出。
这在只有几百行代码的教学项目里还行，
但 Cinux 计划发展到几千行代码的时候，
没有测试就太危险了。

Cinux 从第一个 tag 就引入了自研的轻量测试框架，
支持 Host 模式（在 Linux 上用 g++ 编译运行）
和 QEMU 模式（在内核里通过串口输出），
让逻辑性的 bug 在 Host 端就能被捕获。
Host 模式的优势在于：
不需要内核就能测试纯逻辑
（比如 kprintf 的格式化、GDT 描述符编码、内存管理算法），
QEMU 模式则测试需要硬件交互的部分
（比如中断处理、驱动初始化）。

这种双模式方案的巧妙之处在于：
同一个测试用例的接口完全统一（`TEST` 和 `ASSERT_*` 宏），
底层实现通过条件编译切换，
开发者不需要关心运行环境差异。

---

## 概念精讲

### 为什么不能用 Google Test / Catch2

标准的 C++ 测试框架（Google Test、Catch2、doctest）
都假设了一个 hosted 环境：
它们使用 `malloc` 动态分配内存、
依赖 `<iostream>` 做输出、
使用 `std::exception` 做错误传播、
甚至需要多线程支持。
在内核的 freestanding 环境里，
这些东西统统不存在。

有人可能会说"可以用 Google Test 的 freestanding 裁剪版"。
理论上可行，但实际操作中：
Google Test 即使裁剪后仍然很重，
而且它的构建集成对内核项目来说是个麻烦事
——你需要让 CMake 在两种完全不同的编译环境下
（内核的 freestanding 和测试的 hosted）
同时工作，复杂度飙升。

更简单的方案是用 `assert()` + 手写 main 来做最原始的测试，
但这种方式没有测试发现和注册机制，
测多了之后管理测试列表非常痛苦
——每加一个测试就要手动在 main 里加一行调用，
忘记加就等于白写。

Cinux 的自研框架在两者之间取了一个平衡点：
极简（单头文件，零动态内存，零异常依赖），
但功能足够（自动注册、多种断言宏、Host/QEMU 双模式）。

### TEST 宏的自动注册原理

框架的核心设计是用宏来定义测试用例
（`TEST("name") { ... }`），
利用 C++ 静态对象的构造函数
在 `main` 函数之前执行的特性，
每个 `TEST` 块会自动把自己的函数指针注册到全局数组里。

这里有一个经典的 C 预处理器陷阱：
`__LINE__` 不能直接和标识符拼接。
`##` 运算符（token pasting）不会先展开它的操作数，
所以 `_test_fn_##__LINE__` 会得到字面的 `_test_fn___LINE__`
而不是 `_test_fn_42`。
解决方法是增加一层间接：
定义一个 `_TEST_CAT(a, b)` 宏，
里面再套一个 `_TEST_CAT2(a, b)` 宏做实际的 `##` 拼接。
在调用 `_TEST_CAT` 的时候 `__LINE__` 已经被展开成了数字，
然后 `_TEST_CAT2` 才真正执行拼接。

```
TEST("name") { body } 展开过程：

1. 声明静态函数 _test_fn_42()
2. 定义静态结构体 _TestAutoReg_42
   - 构造函数调用 _register_test("name", file, line, _test_fn_42)
3. 定义静态变量 _test_reg_42（触发构造函数 -> 自动注册）
4. 定义静态函数 _test_fn_42() { body }

关键：_TEST_CAT 两层间接展开

  _TEST_CAT(_test_fn_, __LINE__)
  -> _TEST_CAT2(_test_fn_, 42)    // __LINE__ 在这一步被展开
  -> _test_fn_42                  // ## 在这里执行拼接
```

### ASSERT 宏的设计取舍

断言宏使用 `do { ... } while(0)` 包裹
——这是 C/C++ 宏的标准防御性写法，
确保宏在使用时后面能正确加分号，
不会在 if-else 语境里出问题。

失败时的处理策略是：
打印测试名和断言位置（用 `#expr` 把表达式原样字符串化），
递增全局失败计数，然后 `return`——
注意是 `return` 而不是 `abort`，
这样当前测试会提前退出，
但不会影响后续测试的执行。

这个设计有一个已知的简化缺陷：
`RUN_ALL_TESTS` 的循环在每次测试执行后都打印 `[PASS]`，
即使测试中途断言失败了也会打印。
这是因为循环里没有 per-test 的状态追踪
——要精确追踪需要在每个测试前后保存和比较失败计数。
对于 tag 000 的冒烟测试来说完全够用，后续版本可以改进。

---

## 动手实现

### Step 5: 搭建测试框架

**目标**：实现自研的轻量测试框架，
编写一个冒烟测试验证框架工作正常。

**设计思路**：框架的核心设计是：
用宏来定义测试用例（`TEST("name") { ... }`），
用宏来写断言（`ASSERT_EQ`, `ASSERT_TRUE` 等），
然后在 main 函数里调用 `RUN_ALL_TESTS()` 自动运行所有注册的测试。
框架支持两种运行模式：
Host 模式和 QEMU 模式。
Host 模式用 `-DCINUX_HOST_TEST` 宏启用，
测试代码用系统 g++ 编译在 Linux 上运行，
`printf` 输出结果，`abort()` 处理致命错误。
QEMU 模式下测试代码被编译进内核，
通过串口输出结果。
两种模式共享同一套 `TEST` 和 `ASSERT` 宏接口，
通过条件编译切换底层实现。

**实现约束**：

框架头文件路径是 `test/framework/test_framework.h`。
它需要定义以下核心组件：

一个函数指针类型 `_test_fn_t` 表示测试函数，
一个结构体 `_TestEntry`
（包含名字、文件名、行号、函数指针）
表示测试条目，
Host 模式下使用静态数组 `_test_registry[]`（最多 256 个条目）
通过全局构造函数自动注册，
统计变量 `_tests_passed` 和 `_tests_failed` 记录通过和失败的测试数量，
`TEST` 宏展开为一个静态函数定义加一个自动注册类，
`ASSERT_*` 宏使用 `do { ... } while(0)` 包裹，
失败时打印文件名和行号，
递增失败计数，并 return 跳出当前测试，
`RUN_ALL_TESTS` 函数遍历注册数组，
依次调用每个测试函数，
运行完毕后打印 `[SUITE PASSED]` 或 `[SUITE FAILED]`。

头文件使用单头文件库模式（stb 风格）：
通过 `TEST_FRAMEWORK_IMPL` 宏控制全局变量的定义。
在 `test_smoke.cpp` 的第一行定义这个宏，
然后 `#include` 测试框架头文件，
全局变量就只会被定义一次。

`test/CMakeLists.txt` 配置 Host 模式的测试：
创建可执行目标 `test_smoke`，
源文件只有 `test_smoke.cpp`，
添加 `CINUX_HOST_TEST` 编译定义，
设置包含目录（框架头文件和内核头文件），
注册到 CTest。
需要在顶层 CMakeLists.txt 中用
`CINUX_BUILD_TESTS` option 控制 test 子目录的引入。

冒烟测试（`test/unit/test_smoke.cpp`）
不测试任何内核逻辑，只验证框架本身：
四个测试用例分别覆盖：
整数比较（`ASSERT_EQ/TRUE/FALSE/GT/GE/LE/LT`）、
边界值（零和负数，以及 `ASSERT_NE` 不等断言）、
指针断言（`ASSERT_NULL/NOT_NULL`）、
以及一个空的占位测试（预留未来扩展字符串相关断言）。

**踩坑预警**：`TEST` 宏的展开有一个经典的 C 预处理器陷阱
——`__LINE__` 不能直接和标识符拼接
（`##` 运算符不展开它的操作数），
需要用两层宏间接展开。
具体做法是定义一个 `_TEST_CAT(a, b)` 宏，
里面再套一个 `_TEST_CAT2(a, b)` 宏做实际的 `##` 拼接。
这个问题不处理好，
所有测试用例会生成同一个函数名，
编译直接报重复定义。

另外，`RUN_ALL_TESTS` 里的 per-test 通过/失败追踪需要仔细设计
——每个测试函数失败时 return 而不是 continue，
所以在循环里判断是否失败的方法是
"失败计数是否有变化"
而不是"当前测试是否返回了某个状态"。
当前框架简化了这个问题：
每次循环末尾直接打印 PASS，
如果测试中途断言失败就已经 return 了不会到达这一步。

**验证**：运行 `make test`
（或者 `cd build && ctest --output-on-failure`），
终端应该打印测试运行器标题、测试数量、
每个测试的 `[PASS]` 标记，
最后显示 `Results: 4 passed, 0 failed` 和 `[SUITE PASSED]`。

---

## 构建与运行

完整的从零开始的构建流程如下：

首先确保工具链已安装（Step 1 的检查脚本通过），
然后在项目根目录执行：
创建 build 目录，进入 build 目录，
运行 CMake configure（指定 Debug 模式和开启测试），
最后 `make` 编译。
编译成功后，`make run` 启动 QEMU，`make test` 跑测试。

构建命令：

```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..
make
make test
```

如果需要调试，可以用 `make run-debug` 启动 QEMU
（会在端口 1234 开 GDB server，启动后暂停等待连接），
然后在另一个终端用 GDB 连接：
加载内核 ELF 符号文件，
连接 `target remote :1234`，
设置断点，`continue` 继续。
Tag 000 的 MBR 存根没有符号信息（纯汇编裸二进制），
GDB 调试在后续章节才有实际用途，但可以先验证 GDB 能连上。

QEMU 的常用启动参数说明：
`-m` 指定模拟内存大小，
`-serial stdio` 把虚拟串口映射到宿主终端
（后续章节的 `kprintf` 输出会出现在这里），
`-no-reboot` 让 QEMU 在 triple fault 时退出而不是重启，
`-no-shutdown` 让 QEMU 在执行 `hlt` 后保持运行而不自动退出，
`-s` 开启 GDB stub，
`-S` 启动后暂停。

---

## 调试技巧

### 问题：测试编译失败，报 undefined reference to _test_registry

确保 `test_smoke.cpp` 文件开头定义了
`#define TEST_FRAMEWORK_IMPL`
再 `#include "test_framework.h"`。
这个宏控制全局变量的定义，
不定义的话只有声明没有定义，链接器会报未定义引用。

### 问题：测试运行时所有测试都是 PASS，但预期有失败

检查 `ASSERT_*` 宏是否正确使用了 `do { ... } while(0)` 包裹。
如果宏展开后有问题（比如分号被吃掉了），
断言可能不会真正执行。
另外确认 `_tests_failed` 变量是否被正确定义
（需要在 `TEST_FRAMEWORK_IMPL` 宏保护下）。

### 问题：CMake configure 找不到测试目标

确认顶层 CMakeLists.txt 里 `CINUX_BUILD_TESTS` 被设为 ON：
`cmake -DCINUX_BUILD_TESTS=ON ..`。
默认不引入 test 子目录，
需要显式开启。

---

## 本篇小结

| 概念 | 要点 |
|------|------|
| 测试框架设计 | 单头文件，零动态内存，零异常依赖 |
| TEST 宏 | 两层间接展开解决 `__LINE__` + `##` 陷阱 |
| ASSERT 宏 | `do { ... } while(0)` 包裹，失败时 return |
| Host/QEMU 双模式 | `CINUX_HOST_TEST` 条件编译切换底层实现 |
| `TEST_FRAMEWORK_IMPL` | stb 风格单头文件，控制全局变量定义 |
| CTest 集成 | `add_test` 注册，`ctest --output-on-failure` 运行 |
| 冒烟测试 | 4 个测试用例：整数、边界值、指针、占位 |

到这里，tag 000 的全部工作就完成了：
环境搭建、CMake 构建系统、MBR 存根、测试框架。
从安装工具链到在 QEMU 里看到第一个字符输出，
再到跑通自研测试框架
——这些都是后续所有 tag 的基础。
下一章我们将真正进入 MBR bootloader 的开发：
BIOS 中断、磁盘读取、stage2 加载。
准备好进入实模式的世界吧。

---

## 扩展方向

- 给测试框架加一个 `ASSERT_STREQ(str1, str2)` 宏，
  用 `strcmp` 比较字符串，需要处理空指针的情况
- 给 RUN_ALL_TESTS 加 per-test 的通过/失败追踪，
  修复当前"失败的测试也打印 PASS"的问题
- 尝试把测试框架改成 QEMU 模式
  （定义 QEMU 版的 `serial_printf` stub），
  让测试在内核环境里通过串口输出
- 给 `build_image.sh` 加 stage2 写入逻辑
  （第二个 `dd` 命令），为 tag 001 做准备
- 尝试把构建系统迁移到用 `add_custom_command` 生成依赖图，
  让 CMake 自动跟踪 `mbr.bin` -> `cinux.img` 的依赖关系，
  修改源码后只需要 `make` 就能自动重跑 objcopy + dd

## 参考资料

- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones): freestanding 环境和编译 flag 参考
- Intel SDM — Volume 1: Basic Architecture，
  描述实模式寄存器和 red zone 约束
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式和 BIOS 引导行为
- OSDev Wiki — [Real Mode](https://wiki.osdev.org/Real_Mode): 实模式寻址机制
