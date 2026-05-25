---
title: 测试基建总结
---

# 005-05: 测试基建总结

## 整体架构

Cinux 测试基础设施采用双模式架构，支持在宿主机和真实内核环境中运行测试：

```
┌─────────────────────────────────────────────────────────────┐
│                      Cinux 测试架构                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────┐      ┌──────────────────────┐    │
│  │   Host 端测试       │      │   内核端测试         │    │
│  │   (test/unit/)      │      │   (kernel/mini/test/)│    │
│  ├─────────────────────┤      ├──────────────────────┤    │
│  │ • test_smoke.cpp    │      │ • main_test.cpp      │    │
│  │ • test_kprintf_...  │      │ • test_cpp_basic.cpp │    │
│  └──────────┬──────────┘      └──────────┬───────────┘    │
│             │                            │                  │
│             ▼                            ▼                  │
│  ┌─────────────────────┐      ┌──────────────────────┐    │
│  │  g++ / Linux        │      │  -ffreestanding      │    │
│  │  CINUX_HOST_TEST    │      │  QEMU 内核模式        │    │
│  └──────────┬──────────┘      └──────────┬───────────┘    │
│             │                            │                  │
│             └────────────┬───────────────┘                  │
│                          ▼                                  │
│             ┌────────────────────────────┐                  │
│             │  自研测试框架               │                  │
│             │  test_framework.h          │                  │
│             └────────────┬───────────────┘                  │
│                          ▼                                  │
│             ┌────────────────────────────┐                  │
│             │  CMake / CTest             │                  │
│             │  make test                 │                  │
│             └────────────────────────────┘                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 文件清单

### 测试框架
| 文件 | 描述 |
|---|---|
| [`test/framework/test_framework.h`](https://github.com/CinuxOS/Cinux/blob/main/test/framework/test_framework.h) | 自研轻量测试框架 |

### Host 测试
| 文件 | 描述 |
|---|---|
| [`test/CMakeLists.txt`](https://github.com/CinuxOS/Cinux/blob/main/test/CMakeLists.txt) | Host 测试构建配置 |
| [`test/unit/test_smoke.cpp`](https://github.com/CinuxOS/Cinux/blob/main/test/unit/test_smoke.cpp) | 冒烟测试 |
| [`test/unit/test_kprintf_format.cpp`](https://github.com/CinuxOS/Cinux/blob/main/test/unit/test_kprintf_format.cpp) | 格式化函数测试 |

### 内核测试
| 文件 | 描述 |
|---|---|
| [`kernel/mini/test/CMakeLists.txt`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/CMakeLists.txt) | 内核测试构建配置 |
| [`kernel/mini/test/main_test.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/main_test.cpp) | 内核测试入口 |
| [`kernel/mini/test/test_cpp_basic.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/test/test_cpp_basic.cpp) | C++ 运行时测试 |

### 自动化
| 文件 | 描述 |
|---|---|
| [`scripts/run_all_tests.sh.in`](https://github.com/CinuxOS/Cinux/blob/main/scripts/run_all_tests.sh.in) | 测试运行脚本模板 |
| [`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake) | QEMU 测试配置 |

## 测试覆盖情况

### 已覆盖组件
| 组件 | 测试文件 | 覆盖率 |
|---|---|---|
| 测试框架本身 | `test_smoke.cpp` | ✓ 基础功能 |
| 格式化函数 | `test_kprintf_format.cpp` | ✓ 全部格式符 |
| C++ 运行时 | `test_cpp_basic.cpp` | ✓ 基础特性 |
| kprintf 输出 | `main_test.cpp` | ✓ 格式化验证 |

### 待覆盖组件
| 组件 | 状态 |
|---|---|
| 物理内存管理 (PMM) | ─ |
| 虚拟内存管理 (VMM) | ─ |
| 中断处理 (IDT/IRQ) | ─ |
| 键盘驱动 | ─ |
| 定时器 | ─ |
| 进程/线程 | ─ |

## 测试命令速查表

```bash
# === 构建配置 ===
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build
cd build

# === Host 测试 ===
make test_host              # 运行所有 Host 测试
make test_verbose           # 详细模式
make test_smoke_run         # 冒烟测试

# === 内核测试 ===
make run-kernel-test        # 自动退出
make run-kernel-test-interactive  # 交互式
make run-kernel-test-debug  # 调试模式

# === 统一测试 ===
make test                   # 运行所有测试（推荐）

# === CTest 直接调用 ===
ctest --output-on-failure   # 运行所有测试
ctest -R smoke              # 按名称过滤
ctest -L kprintf_format     # 按标签过滤
```

## 设计亮点

### 1. 双模式复用
同一套格式化代码 (`format.cpp`) 可在 Host 和内核环境中测试：
```cpp
// Host 测试直接引用内核实现
#include "mini/lib/private/format.h"
TEST("kprintf: decimal positive") {
    char buffer[64];
    int len = format_decimal(42, buffer, sizeof(buffer));
    ASSERT_EQ(std::string(buffer), "42");
}
```

### 2. 自动退出机制
使用 QEMU 的 `isa-debug-exit` 设备实现自动化测试：
```asm
outl %eax, $0xf4    # 退出码 = (value << 1) | 1
```

### 3. 平台抽象层
测试框架通过宏实现平台无关：
```cpp
#ifdef CINUX_HOST_TEST
    #define _TEST_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
    #define _TEST_PRINT(fmt, ...) serial_printf(fmt, ##__VA_ARGS__)
#endif
```

## 改进建议

### 短期改进
1. **添加覆盖率工具**：集成 gcov/lcov
   ```cmake
   add_compile_options(-fprofile-arcs -ftest-coverage)
   link_libraries(gcov)
   ```

2. **添加 CI 配置**：创建 `.github/workflows/test.yml`
   ```yaml
   name: Tests
   on: [push, pull_request]
   steps:
     - uses: actions/checkout@v3
     - run: cmake -DCINUX_BUILD_TESTS=ON -B build
     - run: make -C build test
   ```

3. **增加测试断言**：
   - `ASSERT_STREQ(s1, s2)` - 字符串比较
   - `ASSERT_NEAR(a, b, epsilon)` - 浮点近似
   - `ASSERT_IN_RANGE(val, min, max)` - 范围检查

### 中期改进
1. **测试夹具 (Fixture)**：支持测试前/后钩子
2. **参数化测试**：同一测试用例多组数据
3. **Mock 框架**：模拟硬件依赖

### 长期改进
1. **模糊测试**：随机输入测试
2. **性能测试**：基准测试框架
3. **压力测试**：长时间稳定性测试

## 相关链接

- [005-01: 自研测试框架详解](005-01-test-framework.md)
- [005-02: Host 端单元测试](005-02-host-tests.md)
- [005-03: 内核测试基础设施](005-03-kernel-tests.md)
- [005-04: 测试自动化](005-04-test-automation.md)
