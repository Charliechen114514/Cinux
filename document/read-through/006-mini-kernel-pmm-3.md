---
title: 006-mini-kernel-pmm-3 · 物理内存管理
---

# 006-3 测试框架与集成代码走读

## 概览

本文走读 tag 006 中测试和集成相关的代码：`kernel_test.h`（测试框架）、`test_pmm.cpp`（PMM 测试用例）、`CMakeLists.txt` 的构建改动、`main.cpp` 的集成调用、以及 `linker.ld` 和 `boot.S` 中修复入口点位置的改动。这些文件确保 PMM 被正确集成到内核中，并且经过了完整的测试验证。

关键设计决策一览：测试框架使用宏而非模板（简化实现）、模拟 BootInfo 驱动测试、对象库共享源文件、`.text.start` 段保证入口点位置。

---

## 架构图

```
┌──────────────────────────────────────────────────────────┐
│                     构建系统                               │
│                                                          │
│  mini_kernel_common (OBJECT)                             │
│  ├─ boot.S  crt_stub.cpp  serial.cpp                    │
│  ├─ kprintf.cpp  format.cpp                              │
│  └─ pmm.cpp  ← 新增                                      │
│                                                          │
│  ┌─────────────────┐  ┌──────────────────────┐           │
│  │  mini_kernel    │  │  mini_kernel_test    │           │
│  │  ├─ main.cpp    │  │  ├─ main_test.cpp    │           │
│  │  └─ $<OBJ>      │  │  ├─ test_cpp_basic   │           │
│  │                 │  │  ├─ test_pmm.cpp ←新  │           │
│  │                 │  │  └─ $<OBJ>            │           │
│  └─────────────────┘  └──────────────────────┘           │
└──────────────────────────────────────────────────────────┘
```

对象库 `mini_kernel_common` 是核心设计——生产内核和测试内核共享同一批编译后的 `.o` 文件，只是入口点不同。这保证了测试代码和运行代码使用完全一致的编译选项。

---

## 代码精讲

### kernel_test.h — 轻量级测试框架

```cpp
namespace test {
    static int tests_passed = 0;
    static int tests_failed = 0;

    inline void reset() {
        tests_passed = 0;
        tests_failed = 0;
    }

    inline int total() {
        return tests_passed + tests_failed;
    }

    inline bool all_passed() {
        return tests_failed == 0;
    }

    inline int get_total_failed() {
        return tests_failed;
    }
}
```

测试框架的状态管理极其简单——两个静态计数器分别记录通过和失败的用例数。`static` 修饰意味着每个翻译单元有一份独立副本，但因为 `kernel_test.h` 只被测试相关的文件 include（并且测试运行在裸机上、只有一个线程），所以不会出问题。

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
// ... 其他断言宏
```

断言宏的核心是 `TEST_ASSERT(cond)`——它把条件表达式的文本（通过 `#cond` 字符串化）、文件名和行号打印到串口，然后递增失败计数并直接 `return` 跳出当前测试函数。`return` 而不是 `abort()` 的选择是有意的——内核里没有 `abort()`，而且我们希望一个测试失败后还能继续跑其他测试。其他断言（EQ、NE、GT 等）都是 `TEST_ASSERT` 的简单包装。

```cpp
#define RUN_TEST(fn) \
    do { \
        kprintf("[RUN] %s\n", #fn); \
        int _failed_before = test::tests_failed; \
        fn(); \
        if (test::tests_failed == _failed_before) { \
            test::tests_passed++; \
            kprintf("[PASS] %s\n", #fn); \
        } \
    } while(0)

#define TEST_SUMMARY() \
    do { \
        kprintf("\n=== Tests: %d passed, %d failed ===\n", \
                test::tests_passed, test::tests_failed); \
    } while(0)
```

`RUN_TEST` 宏的判断逻辑值得一提：它在调用测试函数前记录当前的失败计数，调用后再检查——如果失败数没变，说明这个测试通过了。这种"快照比较"的方式比在测试函数内部设标志位更简单可靠。`TEST_SUMMARY` 纯粹是格式化输出。

### test_pmm.cpp — PMM 测试用例

```cpp
namespace test_mock {
    static BootInfo s_test_boot_info;
    static MemoryMapEntry s_test_mmap[4];

    BootInfo* create_test_bootinfo() {
        s_test_mmap[0].base = 0x00000000;
        s_test_mmap[0].length = 0x000A0000;
        s_test_mmap[0].type = 1;

        s_test_mmap[1].base = 0x000A0000;
        s_test_mmap[1].length = 0x00060000;
        s_test_mmap[1].type = 2;

        s_test_mmap[2].base = 0x00100000;
        s_test_mmap[2].length = 127 * 1024 * 1024;
        s_test_mmap[2].type = 1;

        s_test_boot_info.mmap_count = 3;
        s_test_boot_info.entry_point = 0xFFFFFFFF80020000;
        s_test_boot_info.kernel_phys_base = 0x20000;
        s_test_boot_info.kernel_size = 0x40000;

        for (uint32_t i = 0; i < 3; i++) {
            s_test_boot_info.mmap[i] = s_test_mmap[i];
        }

        return &s_test_boot_info;
    }
```

测试模拟了 QEMU 128MB 标准内存布局的三个 E820 条目：低 640KB 可用、640KB-1MB 保留、1MB-128MB 主内存。这和生产环境中 QEMU 实际报告的 E820 布局非常接近，确保测试结果有实际参考价值。

六个测试用例覆盖了 PMM 的核心功能路径：

- **链接器符号访问测试**：验证 `&__kernel_size` 得到非零值，`__mini_kernel_end` 在 BSS 段之后——直接覆盖了那个最坑的陷阱
- **初始化测试**：验证初始化后 total > 0、free > 0、free < total 的基本约束
- **单页分配测试**：分配一页、验证地址非零且 4KB 对齐、验证 free 计数减一、释放后验证 free 计数恢复
- **多页分配测试**：批量分配 10 页、全部释放、验证计数一致性
- **边界测试**：释放空地址（静默忽略）、释放越界地址（静默忽略）、双重释放（计数不变）
- **OOM 测试**：使用只有 2MB 内存的 BootInfo，分配到耗尽，验证返回 0

```cpp
extern "C" void run_pmm_tests() {
    TEST_SECTION("PMM Tests");

    RUN_TEST(test_linker_symbols::test_linker_symbol_access);
    RUN_TEST(test_pmm_init::test_initialization);
    RUN_TEST(test_pmm_alloc::test_single_allocation);
    RUN_TEST(test_pmm_multi::test_multiple_allocations);
    RUN_TEST(test_pmm_edge::test_edge_cases);
    RUN_TEST(test_pmm_oom::test_oom);

    TEST_SUMMARY();
}
```

测试入口函数用 `extern "C"` 声明——因为 `main_test.cpp` 中通过 `extern "C"` 声明并调用它，C 链接保证了符号名不会被 C++ 的 name mangling 修改。

### CMakeLists.txt — 构建系统集成

```cmake
add_library(mini_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    driver/serial.cpp
    lib/kprintf.cpp
    lib/private/format.cpp
    mm/pmm.cpp
)
```

PMM 的实现文件 `mm/pmm.cpp` 被加入对象库。这是唯一需要新增的源文件——头文件（`pmm.h`、`mm_defines.h`、`memory_literals.h`）不需要在 CMake 中显式列出，因为它们通过 `#include` 被源文件引用，而 `target_include_directories` 已经把 `kernel/mini/` 目录加入了搜索路径。

对象库的设计保证了一个关键属性：`mini_kernel`（生产内核）和 `mini_kernel_test`（测试内核）共享完全相同的 `.o` 文件。这意味着 PMM 的实现在两种场景下是完全一致的——不存在"测试通过了但实际运行时有 bug"的可能性（至少在 PMM 本身的逻辑层面，排除了构建差异因素）。

### main.cpp — 内核入口集成

```cpp
#include "mm/pmm.h"

// ... 在 mini_kernel_main 中:

using cinux::mini::mm::pmm::init;
init(boot_info);
```

集成非常简洁——include 头文件、using 声明、一行调用。PMM 的 init 函数内部会通过 `kprintf` 输出所有诊断信息，所以不需要额外的打印代码。调用位置在 E820 信息打印之后、无限停机循环之前，这是最自然的时机——此时串口已经可用、BootInfo 已经解析完成。

### linker.ld — 入口点保证

```ld
.text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
    *(.text.start)        /* _start must be first! */
    *(.text .text.*)
    *(.rodata .rodata.*)
}
```

`*(.text.start)` 出现在 `*(.text .text.*)` 之前，这保证了 `.text.start` 段中的内容（即 `_start` 函数）永远被放在 `.text` 输出段的最前面。链接器按通配符的顺序处理输入段，所以先匹配到的 `.text.start` 会被先放置。

### boot.S — 段指定

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    cli
    movb $0x31, %al
    outb %al, $0xE9
    # ...
```

`_start` 被放在专用的 `.text.start` 段中，而不是默认的 `.text` 段。`"ax"` 标志表示这个段是可分配（allocatable）且可执行（executable）的。通过这种"专用段名 + 链接脚本优先放置"的组合，`_start` 的位置不再受其他代码文件链接顺序的影响。

这段代码还有 debugcon 调试标记：`'1'` 表示进入 `_start`，`'2'` 表示栈设置完成，`'3'` 表示 BSS 清零完成，`'4'` 表示全局构造函数调用完成。配合 `_init_global_ctors` 中的 `'{'` 和 `'}'` 标记，以及 Serial 构造函数中的 `'C'`，完整的 debugcon 输出序列是 `1234{...}C...`——通过检查这个序列就能确认启动流程是否完整。

---

## 设计决策

### 决策：对象库 vs 静态库

**问题**：共享代码应该用 CMake 的 OBJECT 库还是 STATIC 库？

**本项目的做法**：使用 OBJECT 库。

**备选方案**：使用 STATIC 库，生产内核和测试内核各自链接同一个 `.a` 文件。

**为什么不选备选方案**：对象库直接传递 `.o` 文件，不经过 `ar` 打包这一步。静态库有一个隐含行为——链接器只从中提取引用到的 `.o` 文件，如果一个 `.o` 中没有被引用的符号，整个 `.o` 都会被忽略。这可能导致某些只含副作用代码（比如全局构造函数注册）的 `.o` 文件被意外丢弃。对象库则把所有 `.o` 文件都传递给链接器，不进行选择性地提取。

不过对象库也有代价——正是它引发了我们在 006-02 笔记中记录的链接顺序 bug。静态库中 `.o` 的顺序由 `ar` 打包顺序决定，而对象库中 `.o` 的顺序由 CMake 传给链接器的顺序决定，后者可能和前者不同。

**如果要扩展/改进**：长期来看，应该让 bootloader 从 ELF header 中读取 entry point 地址（`e_entry` 字段），而不是硬编码跳转地址。这样不管链接顺序怎么变，bootloader 都能正确找到入口点。

### 决策：宏测试框架 vs 模板测试框架

**问题**：测试框架应该用 C 宏实现还是 C++ 模板实现？

**本项目的做法**：使用 C 预处理宏。

**备选方案**：使用 C++ 模板和 lambda 表达式实现类型安全的断言。

**为什么不选备选方案**：内核运行在 `-ffreestanding -fno-exceptions` 环境下，标准库的 `<type_traits>`、`<functional>` 等都不可用。用宏实现虽然牺牲了类型安全，但在 freestanding 环境中是最简单可靠的选择。`#cond` 字符串化操作让失败信息包含了完整的条件表达式文本，调试体验其实不差。

**如果要扩展/改进**：可以添加 `TEST_ASSERT_STREQ`（字符串比较）和 `TEST_ASSERT_MEMEQ`（内存区域比较）等专用断言，让测试代码更简洁。还可以添加测试超时机制——如果某个测试卡在死循环里，定时器中断（将来实现后）可以强制终止它。

---

## 扩展方向

- **测试内核的自动化运行**：编写脚本用 QEMU 的 `-serial file:output.log` 和 `-device isa-debug-exit` 自动运行测试并检查退出码，难度低
- **压力测试**：添加一个反复分配-释放数千次的测试，验证位图状态和计数器长期一致性，难度中等
- **并发测试**：将来引入多核后，在两个 CPU 核上同时调用 alloc/free，验证是否需要加锁，难度高

---

## 参考资料

- OSDev Wiki: [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors) — `.init_array` 段的使用和手动调用方法
- LD Manual: [SECTIONS Command](https://sourceware.org/binutils/docs/ld/SECTIONS.html) — 链接脚本中段的放置规则和通配符顺序
- CMake Documentation: [Object Libraries](https://cmake.org/cmake/help/latest/command/add_library.html#object-libraries) — 对象库的行为和 `$<TARGET_OBJECTS:...>` 的用法
