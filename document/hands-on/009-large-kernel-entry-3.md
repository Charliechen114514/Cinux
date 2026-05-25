---
title: 009-large-kernel-entry-3 · 大内核入口
---

# 009-3 ELF 加载器 Bug 修复 + 集成测试 + 压力测试

## 章节导语

前面两章我们把大内核的启动流程和输出基础设施都搭好了。但在实际集成测试中，我们踩到了一个非常隐蔽的 bug：ELF 加载器在加载大内核时会自毁——把 PT_LOAD 段复制到目标地址时，目标地址恰好就是源缓冲区自身。这个 bug 只有在用真实的大内核 ELF 做集成测试时才会触发，单元测试里手工构造的小 ELF 根本碰不到。

这一章我们要做的事情分为三部分：首先分析并修复 ELF 加载器的 in-place loading bug；然后编写集成测试，确保大内核从磁盘加载到跳转的完整流程正确；最后写一个压力测试——用脚本生成一个 1GB 的巨型 ELF，验证整个加载链路在大文件下仍然工作。完成本章后，你会看到 QEMU 输出 `=== ALL TESTS PASSED ===` 以及 stress test 成功退出。

本章的前置知识是 008 章的 ELF 加载器和 ATA 驱动，以及本章前两篇的大内核启动和输出基础设施。

---

## 概念精讲

### In-place Loading 和地址重叠

当一个 ELF 的 PT_LOAD 段的 `p_paddr`（目标物理地址）与 ELF 文件本身所在的 staging buffer 地址重叠时，就会发生 in-place loading。具体到我们的场景：mini kernel 把大内核 ELF 从磁盘读到 `0x1000000`（staging buffer），然后 ELF 加载器遍历 PT_LOAD 段，把每个段复制到 `p_paddr` 指定的地址。但大内核的第一个 PT_LOAD 段的 `p_paddr` 恰好就是 `0x1000000`——因为大内核链接在 KERNEL_LMA = 0x1000000。

这意味着 `memcpy(dest, src, filesz)` 的 dest 和 src 指向同一块内存的前后不同偏移。问题在于 `memcpy` 的语义要求源和目标不重叠（`__restrict__`），重叠时行为是未定义的。更糟糕的是，`memcpy` 在覆盖目标区域的同时也破坏了 ELF 头和 Program Header——后续迭代需要读的元数据已经面目全非了。

### CRC32 校验

为了检测磁盘读取是否正确，我们在大内核 ELF 文件的末尾附加一个 CRC32 校验值。构建时用 Python 脚本计算整个 ELF 文件的 CRC32 并追加到文件末尾，mini kernel 加载时重新计算并比对。这能排除磁盘读取错误导致的加载失败。

### 压力测试

为了验证大内核加载链路在极端情况下的鲁棒性，我们用 Python 脚本生成一个 1GB 的合成 ELF 文件，包含合理的 ELF 头和 Program Header，然后让 mini kernel 从磁盘加载它。1GB 的文件意味着 mini kernel 需要从磁盘读取大约 2000 个扇区——这对 ATA 驱动的连续读取能力和 ELF 加载器的内存管理都是考验。

---

## 动手实现

### Step 1: 修复 ELF 加载器的 in-place loading bug

**目标**：在 ELF 加载器进入 PT_LOAD 段复制循环之前，先把所有需要的元数据保存到栈上，防止被 `memmove` 覆盖。

**设计思路**：

在 `load_elf()` 函数中，进入加载循环之前做三件事：把 ELF 头中的关键字段（`e_entry`、`e_phnum`、`e_phoff`、`e_phentsize`）拷贝到局部变量；把所有 Program Header 深拷贝到一个栈上的数组中（最多 16 个）；后续循环只读局部数组，不再回访 staging buffer 中的 ELF 头。

同时把所有的 `memcpy` 替换为 `memmove`——当 `p_paddr` 与 staging buffer 重叠时，`memmove` 能正确处理重叠区域的复制（内部判断前后方向选择合适的拷贝策略）。

**实现约束**：

局部数组的大小需要一个上限，比如 16 个 Program Header 就足够了——我们的内核 ELF 通常只有 3-4 个 PT_LOAD 段。如果 `e_phnum` 超过上限需要报错退出。返回入口点地址时使用保存的 `saved_entry`，不再回读 ELF 头。

**踩坑预警**：这个 bug 非常隐蔽！单元测试里手工构造的 ELF 通常把 `p_paddr` 设成栈上的地址，不可能与 staging buffer 重叠。只有集成测试（从磁盘读取真实的大内核 ELF）才能触发。这也是为什么我们在做完单元测试后必须做集成测试。崩溃表现为串口输出在 PT_LOAD 信息之后突然中断——没有任何异常信息，QEMU 直接退出（`-no-reboot` 导致 triple fault 后关机）。

**验证**：

```bash
# 构建测试镜像并运行集成测试
cmake --build build && cd build && make run-kernel-test
# 预期输出包含：
# [ELF] PT_LOAD[0]: ... paddr=0x0x1000000 ...
# [ELF] Loaded segment 0: ...
# [ELF] PT_LOAD[1]: ...
# [ELF] Loaded segment 1: ...
# [ELF] All PT_LOAD segments loaded.
# === ALL TESTS PASSED ===
```

### Step 2: 修复集成测试的二次调用问题

**目标**：修复 `test_entry_address` 测试中二次调用 `load_elf()` 的问题——此时 staging buffer 已被段数据覆盖，ELF magic 不复存在。

**设计思路**：

使用一个文件局部的共享变量 `g_loaded_entry` 在测试用例之间传递数据。`test_load_elf_success` 在加载成功后把入口地址存到这个变量里，`test_entry_address` 直接读取这个变量做断言，不再重复调用 `load_elf()`。

**验证**：

```bash
cd build && make run-kernel-test
# 预期：test_entry_address PASS
```

### Step 3: 编写 CRC32 校验脚本和集成

**目标**：构建时在大内核 ELF 末尾追加 CRC32 校验值，运行时 mini kernel 读取并验证。

**设计思路**：

创建 `scripts/append_crc32.py` 脚本，读取 ELF 文件内容，计算 CRC32 值（使用 Python 的 `zlib.crc32`），将 4 字节校验值追加到文件末尾。在 CMake 构建流程中，`build_image.sh` 脚本在对大内核 ELF 调用 `append_crc32.py` 之后再写入磁盘镜像。

mini kernel 端需要实现 CRC32 计算函数（可以用查表法加速，创建 `kernel/mini/lib/crc32.h`），在加载 ELF 之前读取文件末尾的 4 字节校验值，然后重新计算文件主体（排除最后 4 字节）的 CRC32 做比对。

**踩坑预警**：CRC32 校验值追加在 ELF 文件末尾后，ELF 的 section header table 仍然指向原始位置——这意味着 CRC32 字节不在任何 ELF section 内。解析 ELF 时不受影响，但如果用 `readelf` 或 `objdump` 检查修改后的文件，可能会有警告。这是预期行为。

**验证**：

```bash
cmake --build build && cd build && make run-kernel-test
# 预期输出包含：
# [CRC32] stored=0xXXXXXXXX computed=0xXXXXXXXX
# [PASS] test_big_kernel_crc32::test_crc32_matches
```

### Step 4: 编写内存布局检查脚本

**目标**：创建 `scripts/check_memory_layout.py` 脚本，在构建时自动检查大内核 ELF 的内存布局是否合理。

**设计思路**：

脚本解析大内核 ELF 文件的 program header，检查以下约束：所有 PT_LOAD 段的 `p_paddr` 应该在物理地址的合理范围内（不低于 `KERNEL_LMA`，不超过可用内存）；相邻段之间不应该有太大的间隙；段的 `p_memsz` 和 `p_filesz` 的关系应该合理（`memsz >= filesz`，差值代表 BSS）。如果检查失败，脚本以非零退出码退出，阻止构建继续。

**验证**：

```bash
cmake --build build
# 如果内存布局有问题，构建会失败并显示错误信息
```

### Step 5: 编写 1GB 压力测试

**目标**：用 Python 脚本生成一个 1GB 的合成 ELF 文件，验证加载链路在大文件下的正确性。

**设计思路**：

创建 `scripts/generate_large_elf.py` 脚本，生成一个合法的 ELF64 文件。这个 ELF 需要有正确的 ELF 头（magic、class、machine）、一个 PT_LOAD 段（`p_paddr` 设为合理地址、`p_filesz = p_memsz = 1GB`），以及填充数据。实际填充可以用零字节，关键是 ELF 头和 Program Header 的结构要正确。

CMake 构建系统中定义 `stress-kernel-elf` target 调用脚本生成 ELF，`stress-test-image` target 把这个 ELF 写入磁盘镜像（和 mini kernel test 一起），`run-stress-test` target 运行测试。测试 mini kernel 从磁盘读取这个巨型 ELF，加载到内存，验证入口点地址，然后写入 QEMU 的 `isa-debug-exit` 设备以特定的退出码退出。

**实现约束**：

QEMU 的 `isa-debug-exit` 设备的退出码映射是 `exit_code = (value << 1) | 1`。mini kernel 测试代码写 0 表示成功（QEMU 退出码 1），写 1 表示失败（QEMU 退出码 3）。需要一个 bash wrapper 脚本 (`scripts/qemu_test_wrapper.sh`) 把 QEMU 退出码映射回 pass/fail。

**踩坑预警**：1GB 的 ELF 文件意味着磁盘镜像至少要 1GB+。确保磁盘空间充足。QEMU 的 `-m 8G` 参数确保虚拟机有足够内存来容纳这个巨型内核。压力测试运行时间会比较长（磁盘读取 + 内存清零），耐心等待。

**验证**：

```bash
cmake --build build && cd build && make run-stress-test
# 预期：QEMU 成功退出，退出码表示测试通过
# 串口输出应包含：
# === Stress Test: 1GB kernel ===
# [ELF] All PT_LOAD segments loaded.
# === Stress Test PASSED ===
```

---

## 构建与运行

```bash
# 完整构建（包括 stress test ELF 生成）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 运行集成测试
cd build && make run-kernel-test
# 预期：=== ALL TESTS PASSED (exit code 0) ===

# 运行压力测试
cd build && make run-stress-test
# 预期：stress test 通过，QEMU 正常退出

# 查看所有可用测试 target
cmake --build build --target help | grep -E "test|stress"
```

---

## 调试技巧

### 常见 Bug 1: CRC32 校验不匹配

如果 `stored` 和 `computed` 的 CRC32 不一致，最可能的原因是 `append_crc32.py` 脚本和 C++ 端的 CRC32 计算使用了不同的数据范围。Python 端计算的是整个 ELF 文件的 CRC32（不含追加的 4 字节），C++ 端也应该只计算文件主体部分。

### 常见 Bug 2: 压力测试 QEMU 崩溃

1GB 的内存操作可能触发各种边界情况。检查 mini kernel 的内存配置，确认 `BIG_KERNEL_LOAD_ADDR` 到 `BIG_KERNEL_LOAD_ADDR + 1GB` 的范围内没有其他关键数据结构。QEMU 的 `-m 8G` 确保有足够物理内存。

### 常见 Bug 3: 集成测试输出截断

如果在 `PT_LOAD[0]` 信息之后输出突然中断，说明 ELF 加载器在复制第一个段时崩溃了——回到 Step 1 的 bug 修复，确认保存了头信息并且用了 `memmove` 而不是 `memcpy`。

### 串口日志分析

```bash
# 保存完整的串口输出到文件
cd build && make run-kernel-test 2>&1 | tee test_output.log
# 搜索关键信息
grep -E "PASS|FAIL|ERROR" test_output.log
grep -E "PT_LOAD|Loaded segment" test_output.log
```

---

## 本章小结

| 组件 | 要点 |
|------|------|
| In-place loading | p_paddr 与 staging buffer 重叠时 memcpy 自毁 |
| 修复方案 | 提前保存 ELF 头 + Program Header 到栈；memcpy → memmove |
| CRC32 | Python 追加校验值，C++ 端重新计算比对 |
| 内存布局检查 | check_memory_layout.py 验证 ELF 段布局合理性 |
| 压力测试 | generate_large_elf.py 生成 1GB 合成 ELF |
| isa-debug-exit | QEMU 退出码映射：(value<<1)\|1 |
| 测试隔离 | 共享变量传递加载结果，避免二次调用 load_elf |
