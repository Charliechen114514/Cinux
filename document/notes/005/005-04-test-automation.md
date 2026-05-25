---
title: 测试自动化
---

# 005-04: 测试自动化

## 概述

Cinux 测试基础设施通过 CMake/CTest 和自定义脚本实现了完整的测试自动化。测试分为 Host 端（宿主机）和内核端（QEMU）两类，可统一运行。

## CTest 集成

### 启用 CTest

配置文件：[`test/CMakeLists.txt`](https://github.com/CinuxOS/Cinux/blob/main/test/CMakeLists.txt)

```cmake
enable_testing()
```

### 注册测试到 CTest

```cmake
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES LABELS "smoke")

add_test(NAME kprintf_format COMMAND test_kprintf_format)
set_tests_properties(kprintf_format PROPERTIES LABELS "kprintf_format")
```

## CMake 测试目标

### Host 端测试目标

#### `test_host`
运行所有宿主机单元测试：
```cmake
add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke test_kprintf_format
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)
```

运行：
```bash
make test_host
```

#### `test_verbose`
详细模式运行 Host 测试：
```cmake
add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS test_smoke test_kprintf_format
    COMMENT "Running host tests in verbose mode..."
)
```

运行：
```bash
make test_verbose
```

#### `test_smoke_run`
快捷运行冒烟测试：
```cmake
add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
```

运行：
```bash
make test_smoke_run
```

### 统一测试目标

#### `test`
运行所有测试（Host + 内核）：
```cmake
add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)
```

运行：
```bash
make test
```

## 测试运行脚本

**模板文件**：[`scripts/run_all_tests.sh.in`](https://github.com/CinuxOS/Cinux/blob/main/scripts/run_all_tests.sh.in)

### 脚本结构

```bash
#!/bin/sh
set -e

echo ''
echo '=== Running Host Tests ==='
@CMAKE_CTEST_COMMAND@ --output-on-failure

echo ''
echo '=== Running Kernel Tests ==='
set +e  # 临时关闭 set -e 以捕获 QEMU 退出码
@QEMU_EXECUTABLE@ @QEMU_COMMON_FLAGS_STR@ @QEMU_TEST_EXTRA_FLAGS_STR@ \
    -drive file=@CMAKE_BINARY_DIR@/cinux_test.img,format=raw,index=0,media=disk
QEMU_EXIT=$?
set -e

# isa-debug-exit: exit code = (value << 1) | 1
# 写入 0 → QEMU 退出码 1 = 测试成功
if [ $QEMU_EXIT -eq 1 ]; then
    echo "=== Kernel tests passed ==="
    exit 0
else
    echo "=== Kernel tests FAILED (exit code: $QEMU_EXIT) ===" >&2
    exit 1
fi
```

### 脚本生成

CMake 配置：
```cmake
configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)
```

变量替换：
- `@CMAKE_CTEST_COMMAND@` → `ctest`
- `@QEMU_EXECUTABLE@` → `qemu-system-x86_64`
- `@QEMU_COMMON_FLAGS_STR@` → QEMU 通用标志
- `@QEMU_TEST_EXTRA_FLAGS_STR@` → 测试专用标志
- `@CMAKE_BINARY_DIR@` → 构建目录

## QEMU 测试配置

**配置文件**：[`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake)

### 测试专用 QEMU 标志

```cmake
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)
```

`isa-debug-exit` 设备允许内核通过向端口 `0xf4` 写入来自动退出 QEMU。

### 测试内核目标

#### `test-image`
构建测试磁盘镜像：
```bash
make test-image
```

输出：`build/cinux_test.img`

#### `run-kernel-test`
自动退出模式运行内核测试：
```bash
make run-kernel-test
```

#### `run-kernel-test-interactive`
交互式运行（需要 Ctrl+C 退出）：
```bash
make run-kernel-test-interactive
```

#### `run-kernel-test-debug`
调试模式运行（GDB 端口 1234）：
```bash
make run-kernel-test-debug
```

## 完整测试命令参考

### 构建配置
```bash
# 配置构建（启用测试）
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..

# 仅构建测试
make test_smoke test_kprintf_format mini_kernel_test
```

### Host 测试
```bash
# 运行所有 Host 测试
make test_host

# 详细模式
make test_verbose

# 运行特定测试
make test_smoke_run
./build/test/test_kprintf_format
```

### 内核测试
```bash
# 运行内核测试（自动退出）
make run-kernel-test

# 交互式运行
make run-kernel-test-interactive

# 调试模式
make run-kernel-test-debug
```

### 统一测试
```bash
# 运行所有测试（推荐）
make test
```

### CTest 直接调用
```bash
# 列出所有测试
ctest -N

# 运行所有测试
ctest --output-on-failure

# 运行特定标签的测试
ctest -L smoke
ctest -R kprintf_format

# 并行运行
ctest -j4

# 详细输出
ctest -V
```

## 测试退出码

| 场景 | 退出码 |
|---|---|
| 所有测试通过 | 0 |
| Host 测试失败 | 1 |
| 内核测试失败 | 非 0 非 1 |
| QEMU 崩溃 | 其他 |

## CI/CD 集成建议

虽然项目当前未配置 CI，但测试基建已支持 CI 集成：

```bash
#!/bin/bash
# ci_test.sh

set -e

# 配置
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build
cd build

# 构建
make -j$(nproc)

# 运行测试
make test

echo "All tests passed!"
```

## 调试测试

### Host 测试调试
```bash
# 使用 GDB
gdb ./test/test_smoke
(gdb) run

# 使用 Valgrind 检查内存
valgrind --leak-check=full ./test/test_smoke
```

### 内核测试调试
```bash
# 终端 1：启动 QEMU 调试模式
make run-kernel-test-debug

# 终端 2：连接 GDB
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break mini_kernel_main
(gdb) continue
```

### 串口输出查看
```bash
# 串口输出已重定向到 stdout，直接可见
# 调试控制台输出到 debug.log
cat debug.log
```
