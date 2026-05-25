---
title: 006-mini-kernel-pmm-3 · 物理内存管理
---

# 006-3 集成 PMM 与测试

## 导语

前两篇我们已经实现了内存管理的基础设施和 PMM 的核心逻辑。现在要做的是把所有东西串起来——让构建系统认识新文件、编写单元测试来验证 PMM 的行为、以及修复一个隐藏很深的链接顺序 bug。完成本文后，你将拥有一个经过完整测试的物理内存管理器，以及一个可以在后续 tag 中复用的内核测试框架。

前置知识：前两篇的全部内容，CMake 对象库的基本概念。

---

## 概念精讲

### 为什么需要内核态测试框架

在用户态编程中，我们有 Google Test、Catch2 等成熟的测试框架，跑测试就是一条命令的事。但在内核开发中，情况完全不同：没有标准库、没有 `main` 函数的标准调用约定、没有 `exit()` 系统调用——你的代码运行在裸机上，唯一的输出通道是串口。

所以我们自己搭建了一个极简的测试框架，核心就是一组断言宏和一个测试运行器。断言失败时通过串口打印文件名和行号，测试运行器统计通过/失败的数量。虽然简陋，但对于验证 PMM 这种纯逻辑模块已经足够了。

### CMake 对象库与链接顺序

CMake 的对象库（Object Library）是一种特殊的库类型——它编译源文件生成 `.o` 文件但不做链接。其他目标可以通过 `$<TARGET_OBJECTS:...>` 引用这些 `.o` 文件。这很适合我们的场景：生产内核和测试内核需要共享同一批源文件（boot.S、serial.cpp、pmm.cpp 等），但各自有不同的 main 函数和链接选项。

但对象库有一个隐含的陷阱：当你把 `.o` 文件传给链接器时，它们的排列顺序可能和静态库不同。在我们的案例中，这导致了一个诡异的现象——bootloader 跳转到内核入口地址时，没有落到 `_start`，而是直接跑进了 `mini_kernel_main`，完全跳过了 BSS 清零和全局构造函数调用。串口驱动因为 `base_port` 没被初始化而卡在死循环里，整个内核看起来"能跑但串口没输出"。

### debugcon 调试技巧

QEMU 提供了一个非常方便的调试端口 `0xE9`：向这个端口执行 `outb`，字符会被写入 debugcon 日志文件。这个机制不依赖任何硬件初始化——不需要串口配置、不需要中断、甚至不需要内存管理。它在 CPU 执行的第一条指令就可以用了。我们在 boot.S 的 `_start` 中输出字符 `'1'`、`'2'`、`'3'`、`'4'` 来标记启动阶段，在 `_init_global_ctors` 中输出 `'{'` 和 `'}'`，在 Serial 构造函数中输出 `'C'`。通过检查 debugcon 日志中这些标记的顺序，就能精确追踪启动流程走到了哪里。

启动参数是 `-debugcon file:debug.log`，然后查看日志：

```bash
cat debug.log
# 预期: 1234{...}C...
# 如果看到 12 然后就断了，说明 BSS 清零阶段出了问题
# 如果完全看不到 1234，说明 _start 根本没被执行
```

---

## 动手实现

### Step 1: 创建测试框架头文件

**目标**：在 `kernel/mini/test/` 目录下创建 `kernel_test.h`，提供断言宏和测试运行器。

**设计思路**：测试框架的核心是一组形如 `TEST_ASSERT_EQ(a, b)` 的宏，展开后检查条件，失败时通过 kprintf 打印失败信息并递增失败计数。`RUN_TEST(fn)` 宏负责调用测试函数并在执行前后检查失败计数的变化来判断是通过还是失败。`TEST_SUMMARY()` 打印最终统计。所有状态（通过数、失败数）保存在 `test` 命名空间的静态变量中。

**实现约束**：

- 依赖 `kprintf` 做输出，所以测试只能在串口初始化之后运行
- 断言宏使用 `do { ... } while(0)` 包装，确保在 if/else 中使用时不会出问题
- 测试函数返回 `void`——断言失败时直接 `return`，不抛异常（内核里没有异常支持）
- 提供 `TEST_ASSERT_*` 家族：EQ、NE、GT、GE、LT、LE、TRUE、FALSE、NULL、NOT_NULL
- 测试框架本身是头文件 only，include 就能用

**验证**：编译通过即可，实际运行测试在 Step 4。

---

### Step 2: 编写 PMM 单元测试

**目标**：创建 `test/test_pmm.cpp`，编写覆盖 PMM 核心功能的测试用例。

**设计思路**：测试需要一个可控的 BootInfo 输入，所以我们构造一个模拟的 BootInfo——包含 QEMU 128MB 典型内存布局的 E820 条目。在这个受控环境下，我们可以精确验证分配和释放的行为。测试覆盖以下几个场景：链接器符号的正确访问（验证取地址得到的是非零值）、PMM 初始化后的统计数字、单页分配和释放、多页批量分配和释放、边界情况（释放空地址、释放越界地址、双重释放）、以及 OOM 处理（分配直到耗尽）。

**实现约束**：

- 模拟 BootInfo 使用静态存储，避免动态分配
- 每个测试用例在独立的命名空间中，互不干扰
- 每个测试前重新调用 `pmm::init()` 确保干净状态
- OOM 测试使用一个很小的 BootInfo（只有 2MB 内存），加速分配到耗尽的过程
- 测试入口函数 `run_pmm_tests()` 标记为 `extern "C"` 以避免 name mangling

**踩坑预警**：双重释放测试可能会让你紧张——释放一个已经空闲的页，PMM 应该静默忽略（因为 `free_page` 内部有 `test_bit` 检查）。如果这个测试失败了，说明 `free_page` 的防重复释放逻辑有 bug。

**验证**：等 Step 4 集成运行。

---

### Step 3: 修改构建系统

**目标**：更新 `CMakeLists.txt`，把 PMM 加入共享对象库，把测试加入测试目标。

**设计思路**：生产内核和测试内核共享同一个对象库 `mini_kernel_common`。我们把 `mm/pmm.cpp` 加到这个对象库的源文件列表中，这样两个目标都能使用 PMM。测试目标 `mini_kernel_test` 使用自己专用的 `main_test.cpp` 作为入口点，它依次调用 C++ 运行时测试和 PMM 测试。测试内核也像生产内核一样被转换为 flat binary 供 bootloader 加载。

**实现约束**：

- `mm/pmm.cpp` 加入 `mini_kernel_common` 的 OBJECT 库源文件列表
- `test/test_pmm.cpp` 加入 `mini_kernel_test` 的源文件列表
- 测试入口 `main_test.cpp` 中声明 `extern "C" void run_pmm_tests()` 并在合适位置调用
- 测试内核使用和生产内核完全相同的链接脚本和编译选项
- 测试目标在 `CINUX_BUILD_TESTS` 为 ON 时才构建

**验证**：

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build -S .
cmake --build build
# 确认两个目标都生成了
ls build/kernel/mini/mini_kernel*
# 应该看到 mini_kernel, mini_kernel.bin,
# mini_kernel_test, mini_kernel_test.bin
```

---

### Step 4: 修复链接器脚本和 boot.S

**目标**：确保 `_start` 永远在内核 `.text` 段的最前面。

**设计思路**：前面提到的对象库链接顺序 bug，根源在于 bootloader 硬编码了跳转地址 `0xFFFFFFFF80020000`，期望那里是 `_start`。但当 CMake 对象库改变了 `.o` 文件的链接顺序时，`_start` 可能不再在 `.text` 段的开头。修复方法是两步走：在链接器脚本中添加一个专门的 `.text.start` 段放在 `.text` 的最前面，然后在 boot.S 中把 `_start` 放到这个专用段里。这样不管其他代码怎么排列，`_start` 永远占据 `.text` 的第一个位置。

**实现约束**：

- 链接器脚本的 `.text` 输出段中，`*(.text.start)` 必须出现在 `*(.text .text.*)` 之前
- boot.S 中 `_start` 函数前添加 `.section .text.start, "ax"` 指令（`"ax"` 表示可分配且可执行）
- 不改变 `_start` 的其他逻辑，只是换了段名

**踩坑预警**：修改后一定要验证 entry point 地址。用 `readelf -h build/kernel/mini/mini_kernel | grep Entry` 检查——应该输出 `0xffffffff80020000`。如果输出的是别的地址（比如 `0xffffffff8002012a`），说明 `.text.start` 没生效，`_start` 还是没有排在最前面。

**验证**：

```bash
cmake --build build

# 验证 entry point
readelf -h build/kernel/mini/mini_kernel | grep "Entry point"
# 预期: Entry point address: 0xffffffff80020000

# 验证 _start 确实在该地址
objdump -d build/kernel/mini/mini_kernel | head -5
# 预期第一行: ffffffff80020000 <_start>:
# 预期第一条指令: ffffffff80020000: fa  cli
```

---

### Step 5: 构建并运行全部测试

**目标**：构建生产内核和测试内核，分别在 QEMU 中运行验证。

**设计思路**：生产内核输出 PMM 初始化信息和 E820 内存映射；测试内核则运行所有测试用例并通过 isa-debug-exit 设备返回退出码（0 表示全部通过）。

**验证**：

```bash
# 构建
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build -S .
cmake --build build

# 运行生产内核
qemu-system-x86_64 -m 128M -serial stdio \
  -drive format=raw,file=build/boot/cinux.img -display none 2>/dev/null

# 预期输出包含:
# [MINI] PMM: Total 32768 pages (128 MB), Free 32747 pages (127 MB)

# 运行测试内核（需要用测试内核的 bin 替换 img 中的内核部分）
# 或者直接查看串口输出确认测试通过
```

---

## 构建与运行

完整的构建和测试流程：

```bash
# 清理并重新构建
rm -rf build
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build -S .
cmake --build build

# 验证生产内核
echo "=== Production Kernel ==="
qemu-system-x86_64 -m 128M -serial stdio -display none \
  -drive format=raw,file=build/boot/cinux.img 2>/dev/null | \
  grep -E "(PMM|Mini Kernel)"

# 验证 entry point
echo "=== Entry Point Check ==="
readelf -h build/kernel/mini/mini_kernel | grep "Entry point"
```

---

## 调试技巧

### 排查全局构造函数未调用的问题

如果你遇到串口完全没有输出、或者 `base_port` 为 0 的情况，按以下步骤排查：

1. 检查 debugcon 日志（`-debugcon file:debug.log`），看有没有 `1234` 字符序列
2. 如果没有 `1234`，说明 `_start` 没被执行——检查 entry point 地址
3. 如果有 `1234` 但没有 `{`，说明 `_init_global_ctors` 没被调用——检查 boot.S 中的 call 指令
4. 如果有 `{` 但没有 `C`，说明某个全局构造函数卡住了——检查构造函数逻辑

```bash
# 检查 debugcon 输出
qemu-system-x86_64 -m 128M -serial stdio -debugcon file:debug.log \
  -drive format=raw,file=build/boot/cinux.img -display none 2>/dev/null
cat debug.log
# 预期: 1234{...}C...J...
#        ^^^^ _start 阶段标记
#            ^ 全局构造函数开始
#                  ^ Serial 构造函数
#                     ^ stage2 跳转标记
```

### 检查符号表和段布局

```bash
# 查看所有链接器符号
nm build/kernel/mini/mini_kernel | grep -E "__kernel|__bss|__mini"

# 查看 .text 段的起始内容
objdump -d build/kernel/mini/mini_kernel | head -20

# 查看 .init_array 内容（全局构造函数指针）
objdump -s -j .init_array build/kernel/mini/mini_kernel
```

---

## 本章小结

本文完成了 PMM 的系统集成：构建系统更新、测试框架搭建、PMM 单元测试编写，以及修复了一个由 CMake 对象库引起的隐蔽 bug。这个 bug 的根因是链接顺序改变导致 `_start` 不在预期地址，跳过了全局构造函数调用，串口驱动因此无法初始化。通过添加专用的 `.text.start` 段，我们确保了入口点的位置不受链接顺序影响。

| 修改项 | 说明 |
|--------|------|
| `CMakeLists.txt` | `mm/pmm.cpp` 加入对象库 |
| `test/CMakeLists.txt` | `test_pmm.cpp` 加入测试目标 |
| `test/kernel_test.h` | 轻量级内核测试框架 |
| `test/test_pmm.cpp` | 6 个 PMM 测试用例 |
| `linker.ld` | 添加 `.text.start` 段 |
| `boot.S` | `_start` 放入 `.text.start` 段 |
| `boot/stage2.S` | 简化调试输出 |

现在我们有了一个经过测试的物理内存管理器。下一章（`007_mini_kernel_interrupts`）我们将给内核装上中断处理能力，让它能够响应异常和外部中断。
