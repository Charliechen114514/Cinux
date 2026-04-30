# 005-3 动手篇：测试框架与调试工具链

> 本章完成后的可见效果：`make test` 一条命令依次运行 Host 端单元测试和 QEMU 内核端测试，全部通过后返回退出码 0；同时 VSCode 按 F5 即可启动 GDB 远程调试内核
>
> 前置要求：已完成本系列前两篇（串口驱动 + kprintf 格式化库）

---

## 导语

前两篇我们完成了串口驱动和 kprintf 格式化输出库，内核终于能"说话"了。但说实话，写了几百行内核代码却没有一行测试，心里多少有点不踏实——你怎么知道 `format_decimal` 对 INT64_MIN 的输出是对的？你怎么知道 `%04d` 真的会补零而不是补空格？你怎么知道 Serial 类的全局构造函数在 `.init_array` 段被正确调用之前就已经完成了初始化？

靠肉眼观察 QEMU 输出来验证这些细节，效率低到令人发指——改一行代码、重新构建、启动 QEMU、用眼睛对比输出，这个循环一次就要几十秒，而且人的眼睛看 "0007" 和 "007" 差不多都是一闪而过。我们需要自动化的测试，需要 `make test` 按下去几秒后告诉你"全过"或者"第 37 行断言失败，预期 42 实际 24"。

这一章要做的事情分成三大块。第一块是自研测试框架的设计——一个轻量到极致的、用 `TEST()` 宏和静态构造函数自动注册测试用例的框架，能在 Linux 用户态和裸机内核里用同一套接口。第二块是双模式测试的实际部署——Host 端测试在 Linux 上直接跑，用来验证纯算法代码（格式化函数）；Kernel 端测试在 QEMU 里跑，用来验证 C++ 运行时（构造函数、虚函数、全局对象）。第三块是调试工具链的搭建——从 GDB 远程调试到 VSCode 的可视化集成，再加上这一章里我们发现并修复的两个关键 bug（PDPT 索引和链接脚本 LMA 计算）。

---

## 概念精讲

### 自研测试框架：为什么不用 Google Test

你可能会问：为什么不用现成的 Google Test 或者 Catch2？答案是"内核环境"。Google Test 需要 C++ 标准库、需要异常支持、需要动态内存分配——这些在 freestanding 的内核里全部不可用。就算只拿它跑 Host 端测试，引入几千行的第三方代码就为了测几个格式化函数，也显得杀鸡用牛刀了。

我们的框架设计目标是：一个头文件搞定，零外部依赖，能在 Host 和 Kernel 两种模式下编译运行。Host 模式下用 `printf` 输出测试结果，Kernel 模式下用 `serial_printf`（本质上是 kprintf）输出——通过 `CINUX_HOST_TEST` 宏做条件编译切换。

框架的核心机制是自动注册。`TEST("test name")` 这个宏展开后做了三件事：生成一个唯一的测试函数（名字由行号保证唯一），创建一个静态注册对象（C++ 的全局构造函数会在 main 之前执行其构造函数），在构造函数里把测试函数指针和名字登记到一个全局数组里。这样一来，你只需要在每个测试文件里写 `TEST()` 宏，`main()` 里的 `RUN_ALL_TESTS()` 就会自动找到并执行所有已注册的测试。

断言方面，提供了 `ASSERT_TRUE`、`ASSERT_FALSE`、`ASSERT_EQ`、`ASSERT_NE`、`ASSERT_NULL`、`ASSERT_NOT_NULL`、`ASSERT_GE`、`ASSERT_LE`、`ASSERT_GT`、`ASSERT_LT` 这一套。断言失败时打印测试名、失败的表达式、文件名和行号，然后从当前测试函数 return（不是 abort，其他测试继续跑）。

这个框架的限制也很明显：最多 256 个测试用例、不支持测试夹具（Fixture）、不支持参数化测试、不支持并发执行。但对于一个教学内核项目来说，这些功能并不必要——保持简单才是最重要的。

### isa-debug-exit：让 QEMU 自动退出的魔法

Kernel 端测试有一个绕不开的问题：怎么让 QEMU 在测试跑完后自动退出？如果不退出，QEMU 会一直卡在那里，你的自动化脚本就永远等不到返回值。

QEMU 提供了一个叫 `isa-debug-exit` 的虚拟设备来解决这个问题。在 QEMU 的启动参数里加 `-device isa-debug-exit,iobase=0xf4,iosize=0x04`，然后在内核代码里往端口 0xF4 写入一个值，QEMU 就会退出。退出码的计算公式是 `(value << 1) | 1`——也就是说，你写入 0，QEMU 的退出码是 1（`0 << 1 | 1 = 1`）；写入 1，退出码是 3。这个看起来很奇怪的编码方式是 QEMU 故意设计的：它确保退出码不会是 0（因为 0 通常表示"正常退出"，而 QEMU 希望你能区分"isa-debug-exit 触发的退出"和"正常关机"）。

我们的约定是：内核测试全部通过时写入 0，退出码 1 表示测试成功；如果内核测试内部检测到失败，写入一个非零值（比如 1），退出码变成 3，脚本就能判断出"测试失败"。统一测试脚本里检查 QEMU 退出码是不是 1 就行了。

### PDPT 索引和链接脚本 LMA：两个踩过的巨坑

这一章里我们修复了两个存在于之前 tag 中的 bug，它们都挺有代表性的，值得在这里详细讲一讲。

**第一个 bug：PDPT 索引算错了。** 虚拟地址 0xFFFFFFFF80000000 的页表索引拆解中，PDPT 索引应该是 510 而不是 511。推导过程是这样的：地址 0xFFFFFFFF80000000 的二进制表示中，bit 38 到 bit 30 这 9 位是 PDPT 索引。0x80000000 这个偏移量的 bit 30 是 0——你可以自己算一下，0x80000000 = 2^31，bit 31 是 1，bit 30 是 0。所以 bits 38:30 = `0b111111110` = 0x1FE = 510。如果误写成 511，CPU 在查页表的时候会查到一个全零的条目（Not Present），触发 Page Fault，然后因为没有 IDT 处理程序，直接 Triple Fault 重启。这个 bug 的症状是：identity mapping 能正常工作但 higher-half 地址一访问就崩。修复方法是把 PDPT 条目的偏移从 511*8 改为 510*8。

同时，这个 bug 的修复还揭示了一个关联问题：x86-64 的页表条目是 64 位的，但我们的 `setup_page_tables` 代码运行在 32 位保护模式下（进 Long Mode 之前），只能用 `movl` 写 32 位值。之前只写了低 32 位，高 32 位残留了垃圾数据。修复方法是每写一个低 32 位，紧接着写一个高 32 位的 `movl $0`。

**第二个 bug：链接脚本用 SIZEOF() 计算 LMA。** 链接脚本的 `AT()` 指令指定了一个段的"加载内存地址"（LMA）——即这个段在磁盘镜像中的物理位置。之前的写法是用 `SIZEOF(.text)` 累加来计算后续段的 LMA，比如 `.data : AT(KERNEL_PHYS_BASE + SIZEOF(.text))`。问题在于，`SIZEOF()` 只返回段内容本身的大小，不包含段间对齐产生的填充。如果 `.text` 段因为对齐要求实际占了 4100 字节但 `SIZEOF(.text)` 返回 4096，那 `.data` 的 LMA 就往后偏了 4 字节——所有数据都错位了。这直接导致 `.init_array` 段里存放的全局构造函数指针变成了 0xFFFFFFFF（读到了垃圾数据），调用时 Triple Fault。正确的写法是 `AT(ADDR(.section) - KERNEL_Virt_BASE)`，因为 `ADDR()` 返回的是段的实际虚拟地址（包含了对齐填充），减去内核基址就得到准确的物理地址。用 `readelf -l kernel.elf` 可以验证 LMA 是否正确。

## 动手实现

### Step 1: 搭建自研测试框架

**目标**：在 `test/framework/test_framework.h` 中实现一个头文件级的测试框架，支持 `TEST()` 宏自动注册和 `ASSERT_*` 系列断言。

**设计思路**：框架需要定义以下组件。一个全局数组 `_test_registry` 用来存放测试函数指针、名称、文件位置，数组大小上限 256。一个全局计数器 `_test_count` 记录已注册的测试数量。一个注册函数 `_register_test()` 在数组末尾添加一条记录。

`TEST("name")` 宏展开后创建一个唯一的测试函数（函数名由 `__COUNTER__` 或 `__LINE__` 保证唯一性），然后定义一个静态类，在其构造函数中调用 `_register_test()`。这个静态类在全局构造阶段自动实例化——C++ 标准保证同一编译单元内的静态对象按定义顺序构造，跨编译单元的顺序虽然未定义，但对测试注册来说无所谓，只要 main() 开始之前全部注册完就行。

`RUN_ALL_TESTS()` 宏遍历注册数组，对每个测试调用其函数指针。打印 `[PASS]` 或 `[FAIL]`，最后汇总通过和失败的数量。

平台适配层通过 `CINUX_HOST_TEST` 宏切换。定义了这个宏时（Host 模式），`_TEST_PRINT` 映射到 `printf`，`_TEST_ABORT` 映射到 `abort()`；未定义时（Kernel 模式），映射到 `serial_printf` 和 `hlt` 死循环。

**实现约束**：整个框架放在一个头文件里，使用时需要 `#define TEST_FRAMEWORK_IMPL` 然后包含这个头文件——这是一种单头文件库的常见模式，`TEST_FRAMEWORK_IMPL` 只在一个 `.cpp` 文件里定义，确保全局数组和函数只有一份定义。

**踩坑预警**：自动注册机制依赖全局构造函数的执行。如果你的链接脚本没有正确设置 `.init_array` 段，或者 `boot.S` 没有调用 `_init_global_ctors`，测试框架的静态注册对象就不会被构造，`_test_count` 始终为 0，`RUN_ALL_TESTS()` 什么都不跑然后报告"全部通过"——这是一个非常隐蔽的假通过。确保在 Kernel 模式下手动检查测试总数是否正确。

**验证**：先在 Host 模式下验证框架本身。创建 `test/unit/test_smoke.cpp`，定义几个最简单的测试（比如 `1+1 == 2`、`0 == 0`、`nullptr` 是 NULL），编译运行后应该看到 `[PASS]` 字样。

### Step 2: 部署 Host 端格式化函数测试

**目标**：在 `test/unit/test_kprintf_format.cpp` 中编写 format_decimal、format_hex、format_binary 的全面单元测试，通过 CMake CTest 运行。

**设计思路**：Host 端测试的关键优势在于可以直接 include 内核的实现文件——`format.cpp` 是纯算法代码，不依赖任何硬件，在 Linux 用户态编译运行完全没问题。CMakeLists.txt 里把这个文件加到测试可执行文件的源文件列表中就行了。

测试用例的覆盖要全面。`format_decimal` 需要测试正数、负数、零、INT64_MAX、INT64_MIN。`format_hex` 需要测试零、小值、大值、全 F、大小写切换。`format_binary` 需要测试零、单个 bit、连续多个 bit、全部 bit 为 1。每个测试调用格式化函数后用 `ASSERT_EQ` 比较输出字符串和预期值。

CMake 配置方面，需要 `enable_testing()` 启用 CTest，为每个测试可执行文件创建 `add_test()` 条目，设置 `CINUX_HOST_TEST` 编译宏。

**实现约束**：测试文件需要 `#define TEST_FRAMEWORK_IMPL` 然后包含 `test_framework.h`。include 路径要包含 `test/framework/`（框架头文件所在目录）和 `kernel/`（内核实现文件所在目录，因为 format.cpp 内部的 include 路径是相对于 kernel/ 目录的）。

**踩坑预警**：Host 测试和 Kernel 代码的编译选项差异可能导致一些微妙的行为差异。比如 Host 模式下 `int64_t` 可能是 `long` 而 Kernel 模式下是 `long long`（取决于平台）。虽然在我们的环境里不太会遇到这个问题，但如果你把测试代码移植到 32 位 Host 上跑就要小心了。

**验证**：

```
cd build
make test_kprintf_format
ctest -R kprintf_format --output-on-failure
```

所有测试用例应该显示 `[PASS]`。如果某个断言失败，ctest 会打印具体的失败信息——测试名、断言表达式、文件名和行号。

### Step 3: 部署 Kernel 端 C++ 运行时测试

**目标**：在 `kernel/mini/test/` 目录下实现内核端测试，验证 C++ 运行时特性（构造/析构、虚函数、全局对象、多重继承）在 freestanding 内核环境下的正确性。

**设计思路**：Kernel 端测试需要一个独立的测试入口 `mini_kernel_main()`（替代生产内核的 `kernel_main`），用和 Production 内核完全相同的编译选项（`-ffreestanding`、`-fno-exceptions`、`-fno-rtti`、`-mcmodel=large`、`-mno-red-zone`）来编译。这确保了测试环境就是真实的内核环境——如果测试通过，生产内核的 C++ 运行时也没问题。

测试用例涵盖四个场景。第一个是简单类的构造和析构——创建一个局部对象，验证构造函数和析构函数的调用次数。第二个是虚函数——定义一个基类和派生类，通过基类指针调用虚函数，验证多态行为正确。第三个是全局对象——定义一个静态全局对象，验证它在 `mini_kernel_main()` 之前就被构造好了（依赖于 `_init_global_ctors` 的正确执行）。第四个是多重继承——一个类继承两个基类，各自有纯虚函数，验证方法分派到正确的实现。

内核端的测试框架做了简化——没有用 `TEST()` 宏的自动注册机制（避免增加不必要的复杂度），而是手动的 `TEST_ASSERT` 宏和 `RUN_TEST` 宏。`TEST_ASSERT` 检查条件，失败时通过 kprintf 输出失败信息然后 return；`RUN_TEST` 调用测试函数并打印结果。

测试完成后，通过 `isa-debug-exit` 设备退出 QEMU——往端口 0xF4 写入 0，QEMU 以退出码 1 结束。

**实现约束**：内核测试的 CMakeLists.txt 需要包含和生产内核一样的源文件（boot.S、crt_stub.cpp、serial.cpp、kprintf.cpp、format.cpp），再加上测试入口和测试用例文件。链接脚本也用同一个 `linker.ld`。

**踩坑预警**：`isa-debug-exit` 的退出码计算容易让人困惑。写入 0 对应 QEMU 退出码 1（`0 << 1 | 1 = 1`），脚本里判断退出码是否等于 1 来决定测试是否通过。但如果你在某个测试失败时想用不同的退出码来表示不同的错误类型，要注意避开退出码 1。比如写入 1 对应退出码 3，写入 2 对应退出码 5，等等。

**验证**：

```
cd build
make run-kernel-test
```

QEMU 启动后应该自动退出（不需要 Ctrl+C），终端显示内核测试的串口输出。测试通过时，脚本输出 `=== Kernel tests passed ===`。

### Step 4: 统一测试自动化脚本

**目标**：配置 CMake 的 `make test` 目标，一条命令依次运行 Host 端测试和 Kernel 端测试。

**设计思路**：统一测试脚本 `run_all_tests.sh` 是一个 CMake configure_file 生成的 shell 脚本，它分两步执行。第一步运行 `ctest --output-on-failure` 执行所有 Host 端测试。第二步启动 QEMU 运行内核测试，捕获退出码——退出码 1 表示成功，其他值表示失败。

CMakeLists.txt 里需要定义 `test` 目标（依赖 Host 测试可执行文件和 test-image），以及 `test_host` 目标（只跑 Host 测试）。qemu.cmake 里需要增加 QEMU 测试专用标志（`isa-debug-exit` 设备）和对应的 make 目标（`run-kernel-test`、`run-kernel-test-interactive`、`run-kernel-test-debug`）。

**实现约束**：脚本里在运行 QEMU 之前要 `set +e`（临时关闭遇到错误就退出的行为），因为 QEMU 的退出码 1 对于 shell 来说是"非零退出码"，如果不关 `set -e`，脚本会在测试通过的时候退出。捕获退出码后再 `set -e`，然后检查退出码是否等于 1。

**验证**：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON
cmake --build build
cd build && make test
```

输出应该包含：

```
=== Running Host Tests ===
... (ctest 输出，全部 PASS)

=== Running Kernel Tests ===
... (QEMU 串口输出)
=== Kernel tests passed ===
```

### Step 5: 配置 GDB 远程调试与 VSCode 集成

**目标**：配置 QEMU 的 GDB stub、GDB 初始化命令、以及 VSCode 的 launch.json，实现图形化内核调试。

**设计思路**：QEMU 通过 `-s` 参数在 TCP 端口 1234 启动 GDB stub，`-S` 参数让 CPU 在启动时冻结等待 GDB 连接。GDB 端通过 `target remote :1234` 连接上去，然后就能设断点、单步、查看寄存器和内存。

VSCode 的调试配置使用 `cppdbg` 类型，关键配置项是 `miDebuggerServerAddress: localhost:1234`。`setupCommands` 里需要设置三条 GDB 命令：`set architecture i386:x86-64`（设置目标架构，这对 GDB 17.x 及以上版本是必须的，否则 GDB 无法正确解析 x86-64 的指令和寄存器）、`set disassembly-flavor intel`（让反汇编用 Intel 语法，比 AT&T 好读）、`set pagination off`（禁用分页，避免 GDB 在长输出时暂停等你按回车）。

调试流程是这样的：先在一个终端（或 VSCode 的 task）启动 QEMU 调试模式（`make run-debug`），QEMU 会冻结等待；然后在 VSCode 里按 F5，GDB 连接到 QEMU，在 `_start` 或 `mini_kernel_main` 设断点，`continue` 让 Bootloader 跑起来，在内核入口处断住。

**实现约束**：`.vscode/launch.json` 需要至少两个调试配置——一个用于调试 Mini Kernel（program 字段指向 `build/kernel/mini/mini_kernel`），一个用于调试 Bootloader（需要手动加载 `build/boot/stage2` 的符号文件，因为 Bootloader 没有 ELF 入口点）。

**踩坑预警**：GDB 17.x 版本和 QEMU 的配合有一个已知的坑——不显式设置 `set architecture i386:x86-64` 的话，GDB 会按默认架构（通常是 i386）来解析寄存器和内存，导致 `info registers` 显示的是 32 位寄存器、断点设不准、单步执行跑到错误位置。如果你用 `gdb --version` 发现版本号 >= 16，一定要在 setupCommands 里加上架构设置。另外，Bootloader 阶段的调试（从 MBR 到 Long Mode 切换）GDB 经常会"跟不上"——因为 CPU 模式切换（16 位实模式 -> 32 位保护模式 -> 64 位长模式）会导致 GDB 的寄存器解析出错。解决方案是在已知的模式切换后的地址设断点，用 `continue` 跳过切换过程，而不是 `stepi` 一步步走。

**验证**：按以下步骤手动测试 GDB 调试。

在一个终端启动 QEMU 调试模式：

```
cd build && make run-debug
```

在另一个终端启动 GDB：

```
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) set architecture i386:x86-64
(gdb) break mini_kernel_main
(gdb) continue
```

GDB 应该在 `mini_kernel_main` 处断住，你可以 `list` 查看源代码、`print` 查看变量、`x/10i $pc` 查看反汇编。

### Step 6: 修复 PDPT 索引和链接脚本 LMA

**目标**：修复 `boot/common/long_mode.S` 中的 PDPT 索引错误和 `kernel/mini/linker.ld` 中的 LMA 计算错误。

**设计思路**：

PDPT 索引的修复：在 `setup_page_tables` 函数中，把 PDPT 条目的写入偏移从 511*8 改为 510*8。同时在每个 32 位 `movl` 写入低半部分之后，紧跟一个 `movl $0` 写入高半部分（偏移 +4 字节），确保 64 位页表条目的高 32 位为零。

链接脚本的修复：把所有段的 `AT()` 参数从 `SIZEOF()` 累加方式改为 `ADDR(section) - KERNEL_Virt_BASE`。具体来说，`.text` 的 `AT(KERNEL_PHYS_BASE)` 改为 `AT(ADDR(.text) - KERNEL_Virt_BASE)`（效果相同但更一致），`.data` 的 `AT(KERNEL_PHYS_BASE + SIZEOF(.text))` 改为 `AT(ADDR(.data) - KERNEL_Virt_BASE)`，`.init_array` 同理。同时把 `__init_array_start` 和 `__init_array_end` 移到独立的 `.init_array` 段里，并且对 `.init_array` 的输入段使用 `KEEP()` 防止链接器优化掉看似"没用"的全局构造函数。

**踩坑预警**：`SIZEOF()` vs `ADDR()` 这个坑的隐蔽之处在于，它只在对齐填充存在时才暴露——如果你的 `.text` 段恰好不需要额外对齐（或者对齐填充恰好是 0），`SIZEOF()` 和 `ADDR()` 的差值就是 0，一切看起来正常。但一旦你加了新的代码导致段大小变化、或者改了链接脚本的对齐要求，bug 就冒出来了。所以即使当前的构建碰巧没触发这个 bug，也应该用 `ADDR()` 方式——它永远不会出错。

**验证**：修复后重新构建，用以下命令验证。

检查链接脚本的 LMA 是否正确：

```
readelf -l build/kernel/mini/mini_kernel | head -30
```

LOAD 段的 PhysAddr 应该从 0x20000 开始，连续递增，没有跳跃或重叠。

检查 `.init_array` 的内容：

```
nm build/kernel/mini/mini_kernel | grep init_array
```

`__init_array_start` 和 `__init_array_end` 的地址差应该是全局构造函数数量乘以 8 字节。如果差是 0，说明 `KEEP()` 没生效或者没有 `.init_array` 输入段。

检查页表映射（在 QEMU 启动后通过 debugcon 验证）：如果 Bootloader 的 stage2.S 里加了页表验证代码（读取 0xFFFFFFFF80020000 处的第一个字节并输出到 debugcon），debugcon 日志里应该出现 0xFA（`cli` 指令的操作码），说明 higher-half 映射指向了正确的内核代码。

## 构建与运行

本篇涉及的完整构建和测试流程：

```
# 配置（启用测试）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON

# 构建
cmake --build build

# 运行所有测试
cd build && make test

# 或者分开运行
make test_host                        # Host 端测试
make run-kernel-test                  # Kernel 端测试（自动退出）

# 调试模式
make run-debug                        # QEMU 调试模式（另一个终端）
gdb build/kernel/mini/mini_kernel     # GDB 连接
```

`make test` 的预期输出：

```
=== Running Host Tests ===
Test project /path/to/build
    Start 1: smoke
1/2 Test #1: smoke ........................   Passed
    Start 2: kprintf_format
2/2 Test #2: kprintf_format ..............   Passed

100% tests passed, 0 tests failed

=== Running Kernel Tests ===
[RUN] test_simple_class
[PASS] test_simple_class
[RUN] test_virtual_functions
[PASS] test_virtual_functions
[RUN] test_global_object
[PASS] test_global_object
[RUN] test_multiple_inheritance
[PASS] test_multiple_inheritance
=== Kernel tests passed ===
```

## 调试技巧

### 测试失败时的排查流程

如果 Host 端测试失败了，直接看 ctest 的输出——它会打印失败断言的文件名和行号。因为 Host 端可以用 GDB 直接调试，你可以在测试函数里设断点、单步跟踪、检查中间变量，排查效率非常高。

如果 Kernel 端测试失败了，先检查 QEMU 的退出码。退出码 1 表示 `isa-debug-exit` 正常触发但测试内部可能有问题——看串口输出里有没有 `[FAIL]` 字样。退出码不是 1（比如 0 表示 QEMU 正常关机、其他值表示异常退出），说明内核在测试完成之前就崩了——这时候要用 `make run-kernel-test-debug` 加 GDB 来定位崩溃点。

### readelf 和 nm：链接问题的两大侦探

`readelf -l kernel.elf` 查看 Program Headers，能让你看到每个段的虚拟地址（VirtAddr）和物理地址（PhysAddr）。如果 PhysAddr 不连续或者跳跃了，说明 LMA 计算有问题。`nm kernel.elf | grep init_array` 能看到 `__init_array_start` 和 `__init_array_end` 的地址，如果 `start == end` 说明没有全局构造函数被收集到。

### 双输出策略

在调试内核启动问题时，同时用 `kprintf` 和 `kdebugf` 输出同样的信息。串口输出直接到终端，方便实时观察；debugcon 输出到 `debug.log` 文件，串口挂了（比如 Serial 驱动本身有 bug）的时候 debugcon 日志依然完整。这两个通道的格式化引擎是同一套代码，所以输出内容完全一致，对比两者就能判断问题在格式化层还是输出层。

## 本章小结

| 概念 | 要点 |
|------|------|
| 自研测试框架 | 头文件级、TEST() 宏自动注册、ASSERT_* 断言、CINUX_HOST_TEST 宏切换平台 |
| Host 端测试 | CMake CTest 驱动，直接 include 内核实现文件，快速验证算法正确性 |
| Kernel 端测试 | 相同编译选项、QEMU 内运行、验证 C++ 运行时（构造/析构/虚函数/全局对象/多重继承） |
| isa-debug-exit | 端口 0xF4，退出码 = (value << 1) \| 1，写入 0 对应退出码 1 = 成功 |
| 统一测试脚本 | `make test` 先跑 Host 再跑 Kernel，脚本判断 QEMU 退出码 |
| GDB 远程调试 | QEMU `-s -S` 参数，GDB `target remote :1234`，需设 architecture i386:x86-64 |
| VSCode 集成 | cppdbg 类型，miDebuggerServerAddress: localhost:1234，setupCommands 设置架构和反汇编风格 |
| GDB 17.x 兼容 | 必须显式 `set architecture i386:x86-64`，否则寄存器解析和断点不准 |
| PDPT 索引 bug | 0xFFFFFFFF80000000 的 PDPT 索引是 510 不是 511（bit 30 = 0），同时写高 32 位清零 |
| LMA 计算修复 | `AT(ADDR(section) - KERNEL_Virt_BASE)` 替代 `AT(BASE + SIZEOF(...))`，避免对齐填充导致错位 |
| 验证工具 | `readelf -l` 查 LMA、`nm | grep init_array` 查全局构造函数、debugcon 查启动流程 |
