---
title: 自研测试框架
---

# 005-01: 自研测试框架详解

## 概述

Cinux 项目采用自研的轻量级测试框架，位于 [`test/framework/test_framework.h`](https://github.com/CinuxOS/Cinux/blob/main/test/framework/test_framework.h)。该框架专为嵌入式/内核开发设计，支持双模式运行：
- **Host 模式**：在 Linux 宿主机上运行，使用标准 C 库
- **QEMU 模式**：直接在内核中运行，通过串口输出

## 核心设计理念

### 1. 最小依赖
框架仅依赖基础 C/C++ 语言特性，无需复杂的第三方测试库（如 Google Test）。这使得测试代码可以直接在裸机环境中运行。

### 2. 编译期宏注册
使用宏和 C++ 静态初始化实现自动测试注册：
```cpp
TEST("smoke: 1+1=2") {
    ASSERT_EQ(1 + 1, 2);
}
```

每个 `TEST()` 宏会：
1. 生成一个唯一的测试函数
2. 创建一个静态注册对象
3. 在程序启动前自动注册到测试表

### 3. 双模式抽象层
通过平台适配层隔离输出差异：

```cpp
#ifdef CINUX_HOST_TEST
    #include <stdio.h>
    #define _TEST_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
    #define _TEST_ABORT() abort()
#else
    extern void serial_printf(const char* fmt, ...);
    #define _TEST_PRINT(fmt, ...) serial_printf(fmt, ##__VA_ARGS__)
    #define _TEST_ABORT() do { asm volatile("hlt"); } while (1)
#endif
```

## 断言宏

| 宏 | 说明 |
|---|---|
| `ASSERT_TRUE(expr)` | 验证表达式为真 |
| `ASSERT_FALSE(expr)` | 验证表达式为假 |
| `ASSERT_EQ(actual, expected)` | 验证相等 |
| `ASSERT_NE(actual, expected)` | 验证不等 |
| `ASSERT_NULL(ptr)` | 验证为空指针 |
| `ASSERT_NOT_NULL(ptr)` | 验证非空指针 |
| `ASSERT_GE(a, b)` | 验证 a >= b |
| `ASSERT_LE(a, b)` | 验证 a <= b |
| `ASSERT_GT(a, b)` | 验证 a > b |
| `ASSERT_LT(a, b)` | 验证 a < b |

断言失败时会：
1. 输出失败信息（测试名、断言、文件位置）
2. 增加失败计数
3. 立即从当前测试返回

## 测试注册机制

### Host 模式
使用全局数组和计数器：
```cpp
_TestEntry _test_registry[_MAX_TESTS];  // 最多 256 个测试
int _test_count = 0;
```

每个测试通过静态构造函数自动注册：
```cpp
static struct _TestAutoReg_##__LINE__ {
    _TestAutoReg_##__LINE__() {
        _register_test(test_name, __FILE__, __LINE__, _test_fn);
    }
} _test_reg_##__LINE__;
```

### QEMU 模式
理论上可使用 linker section trick（`.cinux_tests` section），但当前实现与 Host 模式相同。

## 运行测试

```cpp
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
```

`RUN_ALL_TESTS()` 会：
1. 打印测试总数
2. 依次执行每个测试
3. 统计通过/失败数量
4. 输出最终结果

## 使用示例

```cpp
#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

TEST("pmm: alloc single page") {
    void* page = pmm_alloc_page();
    ASSERT_NOT_NULL(page);
    ASSERT_EQ((uintptr_t)page % 4096, 0UL);  // 4K 对齐
    pmm_free_page(page);
}

TEST("pmm: alloc multiple pages") {
    void* pages[3];
    for (int i = 0; i < 3; i++) {
        pages[i] = pmm_alloc_page();
        ASSERT_NOT_NULL(pages[i]);
    }
    for (int i = 0; i < 3; i++) {
        pmm_free_page(pages[i]);
    }
}
```

## 限制与注意事项

1. **最大测试数量**：默认 256 个，可通过修改 `_MAX_TESTS` 调整
2. **不支持参数化测试**：每个测试必须单独编写
3. **不支持测试夹具（Fixture）**：需手动管理测试状态
4. **失败即停止**：断言失败后立即返回当前测试，不会继续执行

## 与其他框架对比

| 特性 | Cinux 框架 | Google Test | Catch2 |
|---|---|---|---|
| 依赖 | 无 | gtest | 头文件库 |
| 二进制大小 | 极小 | 较大 | 中等 |
| 内核适配 | 原生支持 | 需修改 | 需修改 |
| 功能完整度 | 基础 | 完整 | 较完整 |
