---
title: Host 端单元测试
---

# 005-02: Host 端单元测试

## 概述

Host 端测试是在 Linux 宿主机上运行的单元测试，编译时定义 `CINUX_HOST_TEST` 宏。这类测试可以充分利用宿主机的调试工具和标准库，适合快速验证算法逻辑。

## 目录结构

```
test/
├── CMakeLists.txt          # Host 测试构建配置
├── framework/
│   └── test_framework.h    # 自研测试框架
└── unit/
    ├── test_smoke.cpp               # 冒烟测试
    └── test_kprintf_format.cpp      # kprintf 格式化测试
```

## CMake 配置

配置文件：[`test/CMakeLists.txt`](https://github.com/CinuxOS/Cinux/blob/main/test/CMakeLists.txt)

### 关键配置项

```cmake
# 设置测试专用编译定义
set(CINUX_HOST_TEST ON)

# 设置测试包含目录
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

# 启用 CTest
enable_testing()
```

### 测试可执行文件

#### test_smoke - 冒烟测试
```cmake
add_executable(test_smoke unit/test_smoke.cpp)
target_compile_definitions(test_smoke PRIVATE CINUX_HOST_TEST)
target_include_directories(test_smoke PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME smoke COMMAND test_smoke)
```

#### test_kprintf_format - 格式化函数测试
```cmake
add_executable(test_kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)
target_compile_definitions(test_kprintf_format PRIVATE CINUX_HOST_TEST)
target_include_directories(test_kprintf_format PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME kprintf_format COMMAND test_kprintf_format)
```

## 现有测试用例

### 1. 冒烟测试 (test_smoke.cpp)

**文件**：[`test/unit/test_smoke.cpp`](https://github.com/CinuxOS/Cinux/blob/main/test/unit/test_smoke.cpp)

**目的**：验证测试框架本身工作正常

| 测试名称 | 内容 |
|---|---|
| `smoke: 1+1=2` | 基本整数运算和断言宏验证 |
| `smoke: boundary values` | 零值、负数、边界比较 |
| `smoke: pointer assertions` | 空指针、非空指针验证 |
| `smoke: string placeholder` | 字符串测试预留位置 |

**运行**：
```bash
make test_smoke_run
# 或
ctest -R smoke
```

### 2. kprintf 格式化测试 (test_kprintf_format.cpp)

**文件**：[`test/unit/test_kprintf_format.cpp`](https://github.com/CinuxOS/Cinux/blob/main/test/unit/test_kprintf_format.cpp)

**目的**：测试内核格式化函数的正确性

#### 十进制格式化测试
| 测试名称 | 输入 | 预期输出 |
|---|---|---|
| `kprintf: decimal positive` | 42 | "42" |
| `kprintf: decimal negative` | -12345 | "-12345" |
| `kprintf: decimal zero` | 0 | "0" |
| `kprintf: decimal INT64_MIN` | INT64_MIN | "-9223372036854775808" |
| `kprintf: decimal INT64_MAX` | INT64_MAX | "9223372036854775807" |

#### 十六进制格式化测试
| 测试名称 | 输入 | 预期输出 |
|---|---|---|
| `kprintf: hex lowercase` | 0xDEADBEEF | "deadbeef" |
| `kprintf: hex uppercase` | 0xDEADBEEF | "DEADBEEF" |
| `kprintf: hex zero` | 0 | "0" |
| `kprintf: hex all digits` | 0x123456789ABCDEF0 | "123456789abcdef0" |

#### 二进制格式化测试
| 测试名称 | 输入 | 预期输出 |
|---|---|---|
| `kprintf: binary` | 0b101010 | "101010" |
| `kprintf: binary zero` | 0 | "0" |
| `kprintf: binary leading zeros suppressed` | 0b00101 | "101" |
| `kprintf: binary max 64-bit` | 0xFFFFFFFFFFFFFFFF | 64 个 '1' |
| `kprintf: binary power of 2` | 1 << n | "1" + n 个 "0" |

## 运行 Host 测试

### 构建测试
```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..
make
```

### 运行所有 Host 测试
```bash
make test_host
# 或
ctest --output-on-failure
```

### 运行特定测试
```bash
# 冒烟测试
make test_smoke_run
# 或
./test/test_smoke

# 格式化测试
./test/test_kprintf_format
```

### 详细模式
```bash
make test_verbose
# 或
ctest --verbose --output-on-failure
```

## Host 测试的优势

1. **快速反馈**：无需启动 QEMU，测试运行速度快
2. **调试便利**：可使用 GDB、Valgrind 等工具
3. **标准库支持**：可使用 `std::string`、`<limits.h>` 等
4. **CI 友好**：易于集成到持续集成流程

## 编译差异

| 特性 | Host 测试 | 内核测试 |
|---|---|---|
| 编译标志 | `CINUX_HOST_TEST` | 无 |
| 标准库 | 可用 | 不可用 (`-ffreestanding`) |
| 异常 | 支持 | 禁用 (`-fno-exceptions`) |
| RTTI | 支持 | 禁用 (`-fno-rtti`) |
| 输出 | `printf` | `serial_printf` |

## 添加新 Host 测试

1. 在 `test/unit/` 创建 `test_<feature>.cpp`
2. 包含测试框架：
   ```cpp
   #define TEST_FRAMEWORK_IMPL
   #include "test_framework.h"
   ```
3. 编写测试用例：
   ```cpp
   TEST("feature: description") {
       ASSERT_EQ(result, expected);
   }
   ```
4. 在 `test/CMakeLists.txt` 添加目标：
   ```cmake
   add_executable(test_<feature> unit/test_<feature>.cpp)
   target_compile_definitions(test_<feature> PRIVATE CINUX_HOST_TEST)
   target_include_directories(test_<feature> PRIVATE ${TEST_INCLUDE_DIRS})
   add_test(NAME <feature> COMMAND test_<feature>)
   ```
