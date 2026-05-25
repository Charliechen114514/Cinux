---
title: 026-fs-ramdisk-3 · Ramdisk 文件系统
---

# 026-fs-ramdisk-3: Ramdisk 驱动与内核集成

## 导语

前两篇我们把"图纸"（ustar 
格式定义）和"建筑材料"（二进制嵌入管道）都准备好了。现在到了最激动人心的部分——
写一个 Ramdisk 驱动，在内核启动时把嵌入的 initrd 
归档"挂载"起来，遍历里面的每个文件，把文件名和大小打印到串口上。然后把这个驱动
集成到内核的启动序列中，确保它正确工作，同时搭建起完整的测试体系。

完成本篇后，内核启动时你会在串口看到类似这样的输出：


[RAMDISK] Archive at 0x...XXX, size 10240 bytes
[RAMDISK]   FILE: hello.txt  (20 bytes)
[RAMDISK]   FILE: readme.txt  (24 bytes)
[RAMDISK]   DIR:  etc/
[RAMDISK]   FILE: etc/passwd  (13 bytes)
[RAMDISK] 3 file(s) found in initrd.


前置知识：前两篇的内容（ustar 格式定义 + 二进制嵌入管道）。

## 概念精讲

### Ramdisk 驱动的职责

在我们的设计中，Ramdisk 驱动的职责非常清晰：它读取链接器符号定位嵌入的 initrd 
数据，然后按照 ustar 
格式逐条解析头部，对每个有效的条目打印类型和文件名。它不需要提供文件读写功能——
那是后续 VFS（tag 027）的事情。现阶段它是一个"只读枚举器"，帮你确认 initrd 
数据确实正确嵌入了，格式是对的，文件都在。

这个设计看似简陋，但在教学 OS 
的开发过程中非常重要：每一步都要有可验证的输出。如果你直接跳到 VFS 再来调试 
initrd，到时候出了问题你根本分不清是 initrd 数据有问题还是 VFS 逻辑有 bug。

### 链接器符号的声明与使用

embed_binary.sh 
脚本在目标文件中定义了三个全局符号：`_binary_initrd_start`（指向数据起始）、`_
binary_initrd_end`（指向数据末尾）和 `_binary_initrd_size`（数据大小）。在 
C++ 代码中使用这些符号需要用 `extern "C"` 声明它们为 `const uint8_t` 
数组——注意是数组类型不是指针类型，这样符号的地址就是数据本身的地址。

C++ 代码中声明的方式是在匿名 `extern "C"` 块中写两个 `extern const uint8_t 
_binary_initrd_start[]` 和 `extern const uint8_t 
_binary_initrd_end[]`。用末尾减起始得到数据大小：`size = end - start`。

### 数据块对齐与跳转

ustar 归档中，每个文件的数据紧跟在头部后面，但必须按 512 
字节对齐。也就是说，如果文件大小是 100 字节，实际占用 512 
字节（一个块）；如果文件大小是 600 字节，实际占用 1024 
字节（两个块）。跳转到下一个条目的计算公式是：`offset += 512 + ceil(size / 
512) * 512`。

当文件大小为 0 时不占任何数据块，下一个头部紧跟在当前头部后面。目录条目的 
size 通常也是 0，所以目录头部后面直接就是下一个条目。

## 动手实现

### Step 1: 设计 RamdiskEntry 结构

**目标**: 定义描述 ramdisk 中单个文件条目的结构体。

**设计思路**: 虽然当前的 mount 只做枚举打印，但我们需要为未来的 VFS 
集成预留数据结构。RamdiskEntry 存三个信息：文件名指针（指向 ustar 头部的 name 
字段，不是拷贝）、文件大小（从八进制字段解析而来）、文件数据指针（指向头部后面
 512 字节处的数据内容）。

**实现约束**: 结构体放在 `cinux::fs` 命名空间中。name 是 `const 
char*`（指向归档内部，不需要释放），size 是 `uint64_t`，data 是 `const 
void*`。同时定义一个常量 `RAMDISK_NAME_MAX = 
100`，限制打印文件名时的最大长度，防止越界。

**踩坑预警**: name 指针指向归档内部的字符数组，它不一定以 null 终止（虽然 
ustar 规范要求 null 终止，但某些工具生成的归档可能在正好 100 
字节长度时不终止）。打印文件名时必须用有界输出函数，不能直接用 `%s` 格式化。

**验证**: 定义后确认结构体字段类型正确，后续测试会用到它。

### Step 2: 设计 Ramdisk 类

**目标**: 定义 Ramdisk 类的公共接口和私有数据。

**设计思路**: Ramdisk 类设计为"一次性挂载"模式：构造时不需要参数，调用 
`mount()` 时自动从链接器符号获取 initrd 
数据并解析。类的私有成员只有两个：数据起始指针和总大小。公共方法包括 
`mount()`（解析归档，返回文件数）、`base()`（获取起始地址）、`total_size()`（
获取总大小）。

**实现约束**: 类放在 `cinux::fs` 命名空间。私有成员用 C++11 
的类内初始化（`base_{}` 和 `size_{}`），这样构造函数不需要手写。`mount()` 
返回 `uint32_t`（文件条目数），`base()` 返回 `const void*`，`total_size()` 
返回 `uint64_t`。头文件不能包含内核特定头文件（如 kprintf），只依赖标准 
`<stdint.h>` 和 `<stddef.h>`。

**验证**: 在后续测试中实例化 Ramdisk 对象，调用 mount 后检查返回值、base 和 
total_size。

### Step 3: 实现内部辅助函数

**目标**: 在 ramdisk.cpp 的匿名命名空间中实现三个内部辅助函数。

**设计思路**: `is_valid_ustar(const UstarHeader*)` 逐字节比较 magic 字段的前 
5 个字符是否为 "ustar"；`data_blocks(uint64_t size)` 计算文件数据占用的 512 
字节块数，size 为 0 时返回 0，否则向上取整 `(size + 511) / 
512`；`print_bounded(const char*, uint32_t max_len)` 逐字符输出到 kprintf，遇 
null 停止，不超过 max_len 字符。

**实现约束**: 
三个函数都放在匿名命名空间中（内部链接，不导出符号）。`is_valid_ustar` 不用 
`memcmp` 而是手动循环比较，因为头部的 magic 字段后面可能不是 null 
终止的，手动比较更可控。`print_bounded` 用 `kprintf("%c", ...)` 
逐字符输出而不是先构造临时字符串再打印，因为内核环境不适合分配临时缓冲区。

**踩坑预警**: `data_blocks` 在 size 为 0 时必须返回 0，不能走通用公式 `(0 + 
511) / 512 = 0`——等等，这个其实结果也对。但显式处理 size==0 的 case 
更清晰，也能避免未来修改公式时引入 bug。

**验证**: 通过 host 端单元测试验证 is_valid_ustar 对合法/非法/全零 magic 
的判断，data_blocks 对各种大小的块数计算。

### Step 4: 实现 mount() 方法

**目标**: 实现 Ramdisk 类的核心逻辑——挂载并遍历 initrd 归档。

**设计思路**: mount 分七步走。第一步从链接器符号获取 base 和 
size，如果为空或零则打印错误返回 0。第二步进入 while 
循环，条件是剩余空间不小于一个头部大小（512 字节）。第三步检查 
end-of-archive：如果 name[0] 是 null 就 break。第四步验证 ustar 
magic。第五步解析文件大小（octal_to_uint）。第六步根据 typeflag 分类：'0' 或 
'7' 是文件（打印文件名和大小，计数加一），'5' 
是目录（打印目录名）。第七步计算数据块数，跳到下一个条目。

**实现约束**: offset 变量用 `uint64_t`（归档可能很大）。指针转换用 
`reinterpret_cast<const UstarHeader*>(base_ + offset)`。文件名打印用 
`print_bounded` 而不是直接 `%s`。最后打印总文件数。

**踩坑预警**: while 循环的条件 `offset + sizeof(UstarHeader) <= size_` 
不能写成 `offset < 
size_`，否则最后一个不完整的头部会被错误解析。另外，`offset += 
sizeof(UstarHeader) + blocks * USTAR_BLOCK_SIZE` 中的乘法要用 `uint64_t` 
避免溢出。

**验证**: 用内核测试归档（包含 3 个文件）运行 mount，确认返回 3，base 和 
total_size 都正确。用 host 
端测试验证各种边界情况（空归档、单文件、混合文件和目录、错误 magic 等）。

### Step 5: 集成到内核启动序列

**目标**: 在 kernel/main.cpp 的 kernel_main 函数中添加 ramdisk 挂载步骤。

**设计思路**: 在 AHCI 初始化之后、用户态程序启动之前，创建一个 Ramdisk 
对象并调用 mount()。这符合 Cinux 
的渐进式启动策略：先做硬件初始化（串口、GDT、IDT、PIC、VMM、HPET、键盘、PCI、A
HCI），然后做文件系统初始化（ramdisk），最后启动用户态程序。在 mount 
前后加明确的 milestone 打印，方便从串口日志中定位。

**实现约束**: 包含 `kernel/fs/ramdisk.hpp` 头文件。步骤编号为 Step 22，在 
AHCI 初始化（Step 21-22：控制器初始化和 MBR 读取）之后。Ramdisk 对象作为局部变量，不需要全局状态。

**验证**: 运行 QEMU，串口输出中应出现 milestone 026 标记，然后是 `[RAMDISK] 
Archive at ...` 和各文件的列表。

### Step 6: 搭建测试体系

**目标**: 创建内核集成测试和 host 端单元测试。

**设计思路**: 
测试分两层。内核测试（`kernel/test/test_ramdisk.cpp`）验证真实环境：UstarHeade
r 大小、octal_to_uint 正确性、真实 initrd 归档能成功挂载并找到 3 个文件。host 
端测试（`test/unit/test_ramdisk.cpp`）更全面：在内存中手工构造 ustar 
归档，测试所有边界情况——空归档、单文件、目录、混合条目、错误 
magic、数据块跳转、类型过滤等。

**实现约束**: host 端测试需要重新实现内核的纯逻辑函数（因为内核代码依赖 
kprintf 和链接器符号）。测试框架使用 Cinux 已有的 TEST/RUN_TEST 宏。host 
测试中提供一个 `build_ustar_header` 辅助函数来构造测试归档。host 
测试用例必须包括：结构体布局、八进制解析的多种输入、magic 
验证、块数计算、归档遍历（单文件/多文件/混合/空归档/错误magic）、条目类型过滤
。

**踩坑预警**: host 端测试不能直接调用内核的 
Ramdisk::mount()，因为它依赖链接器符号和 
kprintf。解决方案是在测试文件中镜像实现纯逻辑部分（octal_to_uint、is_valid_ust
ar、data_blocks、mount_archive），测试这些镜像函数。内核集成测试则直接调用真实
的 Ramdisk 类。

**验证**: 运行 host 测试 `ctest -R 
ramdisk`，所有用例通过。运行内核测试（QEMU），串口输出显示所有测试 PASS。

## 构建与运行

完整流程：

```bash
cd build && cmake .. && cmake --build . --target big_kernel

# QEMU 运行（内核测试模式）
cmake --build . --target big_kernel_test
# 然后用 QEMU 启动 big_kernel_test，串口输出包含 ramdisk 测试结果

# Host 端单元测试
cmake --build . --target test_ramdisk
ctest -R ramdisk --output-on-failure
```

## 调试技巧

如果 mount() 返回 0 但你确定 initrd 
数据在内核中，检查链接器符号的地址是否合理——用 `nm` 看一下 
`_binary_initrd_start` 
的值，确认它在内核的地址空间范围内。如果地址全零，说明目标文件没被链接进去。

如果 mount() 打印了 "Invalid ustar magic"，用 hexdump 工具检查 initrd.tar 
的前 512 字节，确认偏移 257 处确实是 "ustar"。某些 tar 工具可能生成 GNU 
格式而不是 POSIX ustar 格式，magic 字段的位置可能不同。

如果 mount() 在某个条目后停止了但没遍历完所有文件，检查 data_blocks 
的计算——很可能是跳转偏移量算错了。打印每个条目的 offset 和计算出的下一个 
offset，手动验证是否正确。

## 本章小结

| 概念 | 要点 |
|------|------|
| Ramdisk 类 | 一次性挂载，从链接器符号获取数据，遍历 ustar 条目 |
| mount() 七步 | 获取边界 -> 循环 -> 检查结束 -> 验证 magic -> 解析大小 -> 
分类打印 -> 跳转 |
| 链接器符号声明 | extern "C" const uint8_t _binary_initrd_start[] |
| 有界打印 | print_bounded 防止 name 字段非 null 终止导致越界 |
| 内核集成 | Step 22，在 AHCI 之后、用户态之前 |
| 测试分层 | host 端纯逻辑测试 + 内核集成测试 |

## 构建验证的完整检查清单

在提交代码之前，按照这个检查清单逐项验证：

1. host 端单元测试全部通过：运行 ctest -R ramdisk 
--output-on-failure，确认所有测试绿色。
2. 内核集成测试全部通过：运行 big_kernel_test 的 QEMU 镜像，串口输出显示 
"Ramdisk Tests (026)" 后面
   跟着一连串 PASS。
3. 串口输出正确：在 big_kernel 的 QEMU 运行中，搜索 "[RAMDISK]" 
关键字，确认看到 Archive 地址和大小
   信息、每个文件的名称和大小、最后的文件总数。
4. 文件计数正确：mount 返回值应该是 3（hello.txt、readme.txt、etc/passwd 
三个文件）。
5. 目录也被识别：串口输出中应该有一个 "DIR: etc/" 或类似的目录条目。
6. 无错误信息：确保没有 "Invalid ustar magic" 或 "No initrd archive found" 
之类的错误输出。

## 下一步预告

tag 026 完成后，接下来是 tag 027（VFS——虚拟文件系统）。VFS 会在 ramdisk 
的基础上添加 open/read/write/
close 系统调用框架，让用户态程序能够真正地读取 initrd 
中的文件内容。到时候，shell 里的 cat 命令就能
输出 hello.txt 的内容了。

ramdisk 驱动在 VFS 中会被封装为一个文件系统后端，mount 
返回的文件信息会被组织成 RamdiskEntry 数组，
供 VFS 层查找和读取。我们当前设计的 RamdiskEntry 结构体（包含 name、size 和 
data 指针）已经为这个
扩展做好了准备。
