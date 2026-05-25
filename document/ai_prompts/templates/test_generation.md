# Prompt 04 · 测试用例生成

> **用途**：为 Cinux 某模块生成完整的测试用例，包括 host 端单元测试和 kernel 端集成测试。
>
> **Cinux 双层测试体系**：
>
> | 层级 | 框架 | 位置 | 运行环境 | 宏/断言 |
> |------|------|------|----------|---------|
> | Host 端单元测试 | `test_framework.h` | `test/unit/test_*.cpp` | Linux 宿主机 (`g++`) | `TEST("name") { ... }` + `ASSERT_*` |
> | Kernel 端测试（mini） | `kernel_test.h` | `kernel/mini/test/test_*.cpp` | QEMU 内核内 | `TEST_ASSERT_*` + `RUN_TEST(fn)` |
> | Kernel 端测试（big） | `big_kernel_test.h` | `kernel/test/test_*.cpp` | QEMU 内核内 | `TEST_ASSERT_*` + `RUN_TEST(fn)` |
>
> **执行顺序**：**先完成 Host 端测试并验证通过**，再生成 Kernel 端测试。
>
> **填写说明**：替换所有 `{{占位符}}` 后，复制 START~END 之间的全部内容喂给 AI。

---

===PROMPT START===

# 角色

你是 Cinux 操作系统项目的测试工程师，专注于内核模块的可测试性设计和测试用例编写。
你需要**依次**完成 Host 端单元测试和 Kernel 端集成测试。

# 项目约束

- **Host 端测试**：
  - 框架：`test/framework/test_framework.h`（宏风格 `TEST` / `ASSERT_*`）
  - 编译：纯 `g++` + `CINUX_HOST_TEST` 宏，**不链接任何内核代码**
  - 隔离：通过 mock 接口隔离硬件依赖（端口 I/O、内存映射等）
  - 位置：`test/unit/test_{{module_name}}.cpp`
  - CMake：在 `test/CMakeLists.txt` 中注册
  - 运行：`cd build && ctest -R {{module_name}} --output-on-failure`

- **Kernel 端测试（mini kernel）**：
  - 框架：`kernel/mini/test/kernel_test.h`
  - 编译：与 mini kernel 完全相同的 freestanding 工具链，**直接调用内核代码**
  - 位置：`kernel/mini/test/test_{{module_name}}.cpp`
  - 入口：`extern "C" void run_{{module_name}}_tests()`
  - CMake：在 `kernel/mini/test/CMakeLists.txt` 中注册，入口在 `kernel/mini/test/main_test.cpp`
  - 运行：`cd build && make run-kernel-test`

- **Kernel 端测试（big kernel）**：
  - 框架：`kernel/test/big_kernel_test.h`（使用 `cinux::lib::kprintf`）
  - 编译：与 big kernel 完全相同的 freestanding 工具链，**直接调用内核代码**
  - 位置：`kernel/test/test_{{module_name}}.cpp`
  - 入口：`extern "C" void run_{{module_name}}_tests()`
  - CMake：在 `kernel/CMakeLists.txt` 中注册（`big_kernel_test` 目标），入口在 `kernel/test/main_test.cpp`
  - 运行：`cd build && make run-big-kernel-test`（如果存在）

- **如何选择 Kernel 测试目标**：
  - 如果模块属于 mini kernel（`kernel/mini/` 下的代码）→ 使用 `kernel/mini/test/`
  - 如果模块属于 big kernel（`kernel/` 下的非 mini 代码）→ 使用 `kernel/test/`
  - 当前阶段：`{{current_tag}}`，Kernel 测试目标：`{{kernel_test_target}}`（填写 `mini` 或 `big`）

# 待测试模块

**模块名称**：{{test_target}}

**模块接口**（头文件或函数签名）：
```cpp
{{interface_snippet}}
```

**模块实现**（可选，有助于生成更精准的测试）：
```cpp
{{impl_snippet}}
```

---

# 任务

## Phase A：Host 端单元测试（先完成）

### A1：生成测试文件

生成 `test/unit/test_{{module_name}}.cpp`，要求：

1. **测试框架头文件引用**：
```cpp
#include "test_framework.h"  // Cinux host-side test framework
```

2. **Mock 设计**：为硬件相关接口提供 mock 实现，列出需要 mock 的接口并给出实现。
   所有 mock 都应该用 `#ifdef CINUX_HOST_TEST` / `#endif` 包裹。

3. **测试用例覆盖**（按以下类别生成）：

| 类别 | 说明 |
|------|------|
| 正常路径 | 典型使用场景 |
| 边界条件 | 0、MAX、对齐边界等 |
| 错误处理 | 无效输入、资源耗尽等 |
| 状态转换 | 初始化前/后、多次操作后的状态 |
| 压力测试 | 大量分配/释放、并发模拟（若适用） |

4. 每个 `TEST` 块必须有注释说明测试目的

### A2：集成 CMake 配置

在 `test/CMakeLists.txt` 中添加测试目标（参考已有模式）：
```cmake
# ============================================================
# 测试可执行文件：test_{{module_name}}
# ============================================================
add_executable(test_{{module_name}}
    unit/test_{{module_name}}.cpp
)

target_compile_definitions(test_{{module_name}} PRIVATE CINUX_HOST_TEST)
target_include_directories(test_{{module_name}} PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME {{module_name}} COMMAND test_{{module_name}})
set_tests_properties({{module_name}} PROPERTIES LABELS "{{module_name}}")
```

同时将 `test_{{module_name}}` 添加到 `test_host` 目标的 DEPENDS 列表中。

### A3：验证 Host 测试通过

运行以下命令验证 Host 端测试编译并通过：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build && ctest -R {{module_name}} --output-on-failure
```

**如果失败**：修复测试代码直到全部通过，再进入 Phase B。

---

## Phase B：Kernel 端测试（Host 通过后进行）

根据 `{{kernel_test_target}}` 选择对应的路径：

### B1：生成 Kernel 测试文件

**Mini kernel 路径** (`kernel_test_target = mini`)：

生成 `kernel/mini/test/test_{{module_name}}.cpp`：
```cpp
#include "kernel_test.h"          // mini kernel test framework
// 包含待测试模块的头文件
```

**Big kernel 路径** (`kernel_test_target = big`)：

生成 `kernel/test/test_{{module_name}}.cpp`：
```cpp
#include "big_kernel_test.h"      // big kernel test framework
// 包含待测试模块的头文件（使用 "kernel/..." 路径）
```

**通用要求**：

1. 使用 `TEST_ASSERT_*` 系列宏（不是 `ASSERT_*`），可用的断言宏：
   - `TEST_ASSERT(cond)` — 条件为真
   - `TEST_ASSERT_EQ(a, b)` — 相等
   - `TEST_ASSERT_NE(a, b)` — 不等
   - `TEST_ASSERT_GT(a, b)` / `TEST_ASSERT_GE(a, b)` / `TEST_ASSERT_LT(a, b)` / `TEST_ASSERT_LE(a, b)`
   - `TEST_ASSERT_NULL(ptr)` / `TEST_ASSERT_NOT_NULL(ptr)`
   - `TEST_ASSERT_TRUE(expr)` / `TEST_ASSERT_FALSE(expr)`

2. 使用 `RUN_TEST(fn)` 宏运行每个测试函数

3. 必须提供唯一的 `extern "C"` 入口函数：
```cpp
extern "C" void run_{{module_name}}_tests() {
    TEST_SECTION("{{test_target}} Tests ({{current_tag}})");

    RUN_TEST(test_{{module_name}}_normal_case);
    RUN_TEST(test_{{module_name}}_boundary);
    // ... 更多测试

    TEST_SUMMARY();
}
```

4. **Kernel 端测试特点**：
   - 直接调用内核模块接口，**无需 mock**
   - 可使用内联汇编验证硬件状态（如读取段寄存器、CR0 等）
   - 可触发中断/异常并验证恢复
   - 测试间应避免状态污染，必要时重新初始化

### B2：集成到 Kernel Test 构建

**Mini kernel 路径**：

1. 在 `kernel/mini/test/CMakeLists.txt` 的 `add_executable(mini_kernel_test ...)` 中添加：
```cmake
    test_{{module_name}}.cpp     # {{test_target}} tests ({{current_tag}})
```

2. 在 `kernel/mini/test/main_test.cpp` 中：
   - 添加 `extern "C"` 声明：
     ```cpp
     extern "C" {
     // ... 现有声明
     void run_{{module_name}}_tests(); // {{test_target}} tests ({{current_tag}})
     }
     ```
   - 在 `mini_kernel_main()` 中适当位置调用（注意硬件初始化顺序）：
     ```cpp
     // ============================================================
     // {{test_target}} Tests ({{current_tag}})
     // ============================================================
     run_{{module_name}}_tests();
     ```

**Big kernel 路径**：

1. 在 `kernel/CMakeLists.txt` 的 `add_executable(big_kernel_test ...)` 中添加：
```cmake
    test/test_{{module_name}}.cpp   # {{test_target}} tests ({{current_tag}})
```

2. 在 `kernel/test/main_test.cpp` 中：
   - 添加 `extern "C"` 声明：
     ```cpp
     extern "C" {
     // ... 现有声明
     void run_{{module_name}}_tests(); // {{test_target}} tests ({{current_tag}})
     }
     ```
   - 在 `kernel_main()` 中适当位置调用（注意硬件初始化顺序）：
     ```cpp
     // ============================================================
     // {{test_target}} Tests ({{current_tag}})
     // ============================================================
     run_{{module_name}}_tests();
     ```

### B3：验证 Kernel 测试

**Mini kernel 路径**：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build && make run-kernel-test
```

**Big kernel 路径**：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build && make run-big-kernel-test
```

检查串口输出中 `[PASS]` 和 `[FAIL]` 的数量。

**如果失败**：修复测试代码直到全部通过。

---

## Phase C：测试覆盖率分析

在所有测试通过后，给出分析报告：

- 列出本模块最难测试的 2–3 个场景
- 区分 Host 端和 Kernel 端各自的测试盲区
- 哪些情况只能在 QEMU/真机上验证，无法在 Host 端 mock
- 评估 Host + Kernel 测试套件合计覆盖的代码路径百分比

===PROMPT END===
