---
title: 内核测试基础设施
---

# 005-03: 内核测试基础设施

## 概述

内核测试直接在 QEMU 模拟的 x86_64 环境中运行，验证代码在真实内核条件下的行为。这类测试不依赖宿主机的标准库，使用与生产内核完全相同的编译选项。

## 目录结构

```
kernel/mini/test/
├── CMakeLists.txt          # 内核测试构建配置
├── main_test.cpp           # 测试入口点
└── test_cpp_basic.cpp      # C++ 运行时测试
```

## 编译配置

**配置文件**：[`kernel/mini/test/CMakeLists.txt`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/CMakeLists.txt)

### 源文件组合
```cmake
add_executable(mini_kernel_test
    ../arch/x86_64/boot.S       # 引导汇编
    ../arch/x86_64/crt_stub.cpp # C++ 运行时支持
    main_test.cpp                # 测试入口
    ../driver/serial.cpp        # 串口驱动
    ../lib/kprintf.cpp          # 内核 printf
    ../lib/private/format.cpp   # 格式化函数
    test_cpp_basic.cpp          # C++ 测试
)
```

### 编译选项
与生产内核完全相同：
```cmake
target_compile_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})
```

这些选项包括：
- `-ffreestanding`：自由独立环境，无标准库
- `-fno-exceptions`：禁用异常
- `-fno-rtti`：禁用运行时类型信息
- `-fno-pie`：禁用位置无关可执行文件
- `-mcmodel=large`：大代码模型
- `-mno-red-zone`：禁用 red zone

### 链接配置
```cmake
target_link_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})
```

链接特点：
- 使用自定义链接脚本 `linker.ld`
- 虚拟地址：`0xFFFFFFFF80000000` (higher-half kernel)
- 输出格式：扁平二进制 (`.bin`)

## 测试入口点

**文件**：[`kernel/mini/test/main_test.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/main_test.cpp)

### mini_kernel_main() 函数

这是内核测试的主入口点（替代生产内核的 `kernel_main`）：

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    // 1. kprintf/kdebugf 测试
    kprintf("=== kprintf Test ===\n");
    kprintf("String: %s\n", "Hello, Cinux!");
    kprintf("Decimal: %d\n", -12345);
    // ... 更多格式测试

    // 2. C++ 运行时测试
    run_cpp_tests();

    // 3. 安全退出
    __asm__ volatile("outl %0, $0xf4" : : "a"(0));  // QEMU isa-debug-exit
}
```

### QEMU 安全退出机制

使用 `isa-debug-exit` 设备实现自动退出：
```asm
outl %eax, $0xf4
```

- 端口 `0xf4`：QEMU 的 isa-debug-exit 设备
- 退出码计算：`(value << 1) | 1`
- 写入 0 → QEMU 退出码 1 → 测试成功

## C++ 运行时测试

**文件**：[`kernel/mini/test/test_cpp_basic.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/test_cpp_basic.cpp)

### 测试框架适配

内核环境无法使用标准测试框架，因此实现了一套简化版本：

```cpp
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kprintf("[FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            test::tests_failed++; \
            return; \
        } \
    } while(0)

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

### 测试用例

#### 1. 简单类构造/析构 (test1)
```cpp
class SimpleClass {
    int value;
    char marker;
public:
    SimpleClass(int v) : value(v), marker('S') {
        constructor_calls++;
    }
    ~SimpleClass() {
        destructor_calls++;
    }
};
```
验证：构造函数和析构函数正确调用

#### 2. 虚函数 (test2)
```cpp
class Base {
    virtual char getName() = 0;
    virtual int compute() = 0;
};

class Derived : public Base {
    char getName() override { return 'D'; }
    int compute() override { return multiplier * 2; }
};
```
验证：多态行为、虚函数表

#### 3. 全局对象构造 (test3)
```cpp
static GlobalCounter global_counter;  // main 前构造
```
验证：静态初始化、全局构造顺序

#### 4. 多重继承 (test4)
```cpp
class Multi : public Base1, public Base2 {
    int f1() override { return 11; }
    int f2() override { return 22; }
};
```
验证：多重继承的方法分派

## 内核测试支持组件

### C++ 运行时 Stub

**文件**：[`kernel/mini/arch/x86_64/crt_stub.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/arch/x86_64/crt_stub.cpp)

```cpp
// 纯虚函数调用处理
extern "C" void __cxa_pure_virtual() {
    kdebugf("Pure virtual function called!\n");
    while (1) { asm volatile("hlt"); }
}

// 栈保护失败处理
extern "C" void __stack_chk_fail() {
    kdebugf("Stack check failed!\n");
    while (1) { asm volatile("hlt"); }
}

// 全局构造函数初始化
extern "C" void _init_global_ctors() {
    for (void (**func)() = &__init_array_start; func != &__init_array_end; func++) {
        (*func)();
    }
}
```

### 串口驱动

**文件**：[`kernel/mini/driver/serial.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/serial.cpp)

- NS16550A/8250 UART 兼容
- COM1: 0x3F8 (QEMU 默认)
- 轮询 I/O，无中断支持

## 运行内核测试

### 构建测试内核
```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make mini_kernel_test
```

输出：`build/kernel/mini/mini_kernel_test.bin`

### 直接运行
```bash
qemu-system-x86_64 \
    -kernel build/kernel/mini/mini_kernel_test.bin \
    -nographic \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

### 使用 Make 目标
```bash
# 自动退出模式
make run-kernel-test

# 交互式模式（Ctrl+C 退出）
make run-kernel-test-interactive

# 调试模式
make run-kernel-test-debug
```

## 内核测试 vs Host 测试

| 特性 | 内核测试 | Host 测试 |
|---|---|---|
| 运行环境 | QEMU 内核模式 | Linux 用户态 |
| 标准库 | 不可用 | 可用 |
| 输出方式 | 串口 | stdout |
| 调试难度 | 较高 | 较低 |
| 启动时间 | 秒级 | 毫秒级 |
| 真实性 | 高（真实内核环境） | 中（模拟环境） |

## 添加新内核测试

1. 在 `kernel/mini/test/` 创建 `test_<feature>.cpp`
2. 使用 `kprintf` 输出测试结果
3. 在 `main_test.cpp` 中调用测试函数
4. 更新 `CMakeLists.txt` 添加新源文件
