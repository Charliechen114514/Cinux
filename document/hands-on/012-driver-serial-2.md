# 012-2 kprintf 引擎重构：提取、对齐修饰符与单元测试

## 导语

上一篇我们把 Serial 驱动独立成了正式的驱动模块，kprintf 也完成了格式化引擎与 Serial 的 lambda 集成。但如果你对比一下大内核（`kernel/lib/kprintf.cpp`）和小内核（`kernel/mini/lib/kprintf.cpp`）的代码，你会发现一个非常尴尬的事实——两个文件里的格式化引擎几乎是复制粘贴的。同样的十进制转换、同样的十六进制转换、同样的格式说明符解析，写了两遍，还各自维护。这在软件工程里叫做 DRY 违反（Don't Repeat Yourself），在内核开发里尤其危险——因为将来你要加一个新的格式说明符（比如 `%b` 二进制输出），必须同时改两个文件，漏一个就是 bug。

这一篇我们要做三件事：把格式化引擎提取成一个共享的头文件模板（`vkprintf_impl.hpp`），给这个引擎加上左对齐修饰符（`%-Nd`、`%-Ns`）的支持，然后把单元测试彻底重写——让测试直接使用真正的格式化引擎而不是手写的 mock 副本。完成这一篇后，你会拥有一个在 host 端就能跑测试的格式化引擎，以后任何格式说明符的修改都能通过 `ctest` 自动验证，不需要每次都启动 QEMU。

前置知识方面，你需要了解上一篇的 Serial 驱动和 kprintf 的基本架构。对 C++ 模板有基本了解就够了——我们用的模板非常简单，就是一个接受回调函数的泛型函数。

---

## 概念精讲

### DRY 原则与内核代码共享

DRY（Don't Repeat Yourself）是软件工程里最基本的信条之一——每一份知识在系统里应该只有一个明确的表示。在内核开发里，这个原则说起来容易做起来难。大内核和小内核是两个独立的编译目标，它们链接不同的源文件、运行在不同的地址空间（小内核在 0x200000 物理地址，大内核在 0x1000000 高半核虚拟地址），但它们需要共享一些纯粹的逻辑代码——比如格式化引擎。

Cinux 的解决方案是把格式化引擎提取成一个头文件模板（`vkprintf_impl.hpp`），放在 `kernel/lib/private/` 目录下。这个头文件不包含任何硬件相关的代码——不 include `io.hpp`，不 include `serial.hpp`，它只依赖 `<stdarg.h>` 和 `<stdint.h>` 这两个语言级别的头文件。大内核的 `kprintf.cpp` 和小内核的 `kprintf.cpp` 各自 include 这个共享头文件，然后传入自己的输出回调函数。

这个设计的关键洞察是：格式化引擎接受一个 `OutputFn` 回调——任何可调用的东西，只要能接受一个 `char` 参数就行。串口输出传 `g_serial.putc`，debugcon 输出传 `outb_debugcon`，单元测试传一个往 `std::string` 追加字符的 lambda。格式化引擎完全不知道字节最终去了哪里，它只负责解析格式字符串和生成字符。

### 模板化 OutputFn 的精妙之处

如果你对 C++ 模板不太熟悉，可能会觉得"接受任意回调函数"听起来很复杂。实际上它极其简单——`vkprintf_impl` 是一个函数模板，模板参数 `OutputFn` 可以是任何类型。调用的时候编译器会根据你传入的实参自动推导 `OutputFn` 的类型。如果传的是 lambda，`OutputFn` 就是那个 lambda 的编译器生成类型；如果传的是函数指针，`OutputFn` 就是函数指针类型。

这种设计比传统的"传函数指针 + void* ctx"方式有几个好处。第一，lambda 可以内联——编译器在优化的时候可以看到 lambda 的完整实现，有机会把回调调用完全内联掉，生成和手写循环一样高效的代码。第二，lambda 可以捕获状态——比如测试里的 MockOutput 可以是一个捕获了 `std::string` 引用的 lambda，不需要额外的 context 参数。第三，类型安全——编译器在编译期就能检查 OutputFn 的签名是否正确，而不是运行时才发现函数指针类型不对。

### 左对齐修饰符：`%-Nd` 和 `%-Ns`

标准 printf 的格式说明符语法是 `%[flags][width][.precision][length]type`。Cinux 的 kprintf 不需要实现完整的 printf——那太复杂了，也没有必要。但基本的宽度控制和对齐方式是很有用的，尤其是在打印表格数据或者对齐日志的时候。

这个 tag 新增的左对齐修饰符（`-` flag）的工作方式是这样的：对于 `%-10d`（最小宽度 10，左对齐），如果格式化后的数字只有 4 个字符，引擎会在数字后面补 6 个空格，而不是像默认的右对齐那样在前面补空格。对于 `%-20s`（最小宽度 20，左对齐字符串），如果字符串只有 5 个字符，引擎会在字符串后面补 15 个空格。

实现上，解析器在遇到 `%` 之后先检查是否有 `-` 标志，如果有就设置 `left_align = true`。然后检查是否有 `0` 标志（零填充），然后解析宽度数字，然后解析长度修饰符（`l`、`ll`），最后根据类型字符走不同的分支。在输出阶段，左对齐和右对齐的区别就是"先输出内容再补 padding"还是"先补 padding 再输出内容"。

有一个边界情况值得特别说明——`%06d` 配合负数时的行为。比如 `%06d` 格式化 -42，你期望的输出应该是 `-00042`（符号在最前面，中间补零，最后是数字），而不是 `000-42`（零填充跑到符号前面去了）。为了正确处理这个情况，格式化引擎在遇到"右对齐 + 零填充 + 有符号"的组合时，会先把负号输出，然后补零，最后输出数字部分。这个顺序很关键——写错了会变成一个虽然能编译通过但输出明显错误的 bug。

---

## 动手实现

### Step 1: 提取格式化引擎到共享头文件

**目标**：创建 `kernel/lib/private/vkprintf_impl.hpp`，把 `kprintf.cpp` 里的 `format_decimal`、`format_hex` 和 `vkprintf_impl` 模板全部提取出来，放到 `cinux::lib::detail` 命名空间下。

**设计思路**：提取工作的核心是"剥离硬件依赖"。原来在 `kprintf.cpp` 里，格式化函数和 Serial 输出是紧耦合的——`vkprintf` 直接调用 `g_serial.putc`。提取之后，`vkprintf_impl` 变成一个函数模板，接受一个 `OutputFn` 参数作为输出回调。所有格式化函数（`format_decimal`、`format_hex`）是纯计算函数——接受一个数值和缓冲区，返回格式化后的字符串，不涉及任何 I/O 操作，所以它们可以直接搬过来，不需要修改。

**实现约束**：共享头文件的结构应该是这样的。首先包含 `<stdarg.h>` 和 `<stdint.h>` 两个标准头文件。在 `cinux::lib::detail` 命名空间内定义两个格式化辅助函数：`format_decimal(int64_t value, char* buffer, int buffer_size)` 返回写入的字符数，需要处理 INT64_MIN 的特殊情况（因为 `-INT64_MIN` 会溢出）；`format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase)` 处理十六进制，`lowercase` 参数控制使用 a-f 还是 A-F。这两个函数都使用"先逆序写入临时数组再翻转"的经典手法来生成数字字符串。

然后定义核心的 `vkprintf_impl` 函数模板，模板参数 `OutputFn` 是一个可以调用 `void(char)` 的类型。函数签名接受 `OutputFn&& putc_fn`、`const char* fmt`、`va_list args` 三个参数。函数体就是格式字符串的解析循环——遇到普通字符直接输出，遇到 `%` 就解析格式说明符。

头文件名用 `private/` 子目录是有意为之的——这是一个内部实现细节，不应该被外部的公开头文件 include。只有 `kprintf.cpp` 和测试文件才应该 include 它。

**踩坑预警**：提取的时候最容易犯的错误是忘记处理 va_list 的语义。`va_list` 在某些 ABI 上是一个数组类型（比如 x86_64 的 `va_list` 实际上是 `__va_list_tag[1]`），这意味着按值传递 `va_list` 的行为可能和你预期的不一样。在 Cinux 的实现里，`vkprintf_impl` 接受 `va_list` 按值传递，调用者（`kprintf`、`kvprintf`）用 `va_start`/`va_end` 管理生命周期。这个设计在 GCC 的 x86_64 目标上是安全的，但如果将来移植到其他架构可能需要注意 va_list 的传递语义。

**验证**：提取后重新构建，所有串口输出应该和之前完全一致。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
# 预期：所有输出和提取前完全一样，没有任何变化
# 特别注意 int $3 触发后的寄存器 dump 中的 %p 和 %d 格式化是否正确
```

### Step 2: 实现左对齐修饰符

**目标**：在 `vkprintf_impl` 的格式解析器中添加 `-` 标志的解析，并在 `%d`、`%u`、`%x`、`%X`、`%s` 的输出分支中实现左对齐逻辑。

**设计思路**：格式说明符的解析顺序是固定的——`%` 之后依次检查 `-`（左对齐标志）、`0`（零填充标志）、宽度数字（0-9 序列）、长度修饰符（`l`、`ll`），最后是类型字符。左对齐标志只需要一个 bool 变量来记录。在输出阶段，对于右对齐的情况先输出 padding 再输出内容，对于左对齐的情况先输出内容再输出 padding。对于 `%s` 的左对齐，需要先测量字符串长度，然后决定补多少空格。

**实现约束**：解析阶段在遇到 `%` 之后（消费掉 `%` 之后），首先检查 `*fmt == '-'`，如果是就设置 `left_align = true` 并前进一步。然后检查 `*fmt == '0'` 设置 `zero_pad`。然后解析宽度数字。这些标志的解析顺序不能乱——`%-10d` 里的 `-` 必须在宽度 `10` 之前被消费。

对于 `%d`（有符号十进制）的左对齐，有一个特殊的边界情况需要处理：当 `left_align = false` 且 `zero_pad = true` 且值为负数时，需要先输出负号，然后补零，最后输出数字——也就是前面说的 `%06d` 配合 -42 应该得到 `-00042` 而不是 `000-42`。这意味着 `%d` 的输出分支需要三个子路径：右对齐+零填充+有符号（先符号再零填充再数字）、右对齐普通（先 padding 再整个内容）、左对齐（先内容再空格 padding）。

对于 `%s` 的左对齐，先测量字符串长度（逐字符遍历到 `\0`），然后如果 `left_align` 为 true，先输出字符串所有字符，再补空格到指定宽度；如果为 false，先补空格再输出字符串。

**踩坑预警**：左对齐的 padding 字符永远是空格——即使你同时指定了 `0` 标志。也就是说 `%-010d` 等价于 `%-10d`，零填充标志在左对齐模式下应该被忽略。这是因为左对齐的语义就是"内容靠左、右边补空格"，如果你用零来填充，数字的可读性会被破坏。如果你发现左对齐的数字后面跟了一堆零而不是空格，检查一下是不是在左对齐的分支里也用了 `zero_pad` 标志。另外，`%06d` 和负数的组合是最容易出错的边界情况，一定要写测试覆盖。

**验证**：这一步最好在单元测试里验证（下一步会写），但也可以直接在 kernel_main 里加一行格式化输出来手动检查。

```bash
cmake --build build -j$(nproc)
cd build && make run
# 在 kernel_main 里添加一行测试输出，检查对齐效果
# 预期：左对齐的数字和字符串后面跟的是空格，不是零
# 右对齐+零填充+负数的输出格式应该是 -00042 而不是 000-42
```

### Step 3: 重写单元测试——用真正的格式化引擎

**目标**：重写 `tests/unit/test_kprintf.cpp`，让它直接 include `kernel/lib/private/vkprintf_impl.hpp`，使用 MockOutput 类捕获输出，测试所有格式说明符包括新增的左对齐。

**设计思路**：旧版的单元测试有一个严重的问题——它复制了格式化辅助函数然后在测试文件里重新实现了一个 mock vprintf。这意味着测试测的不是真正的格式化引擎，而是一份"可能已经和真实代码不同步"的副本。新版的测试直接 include 共享头文件，使用真正的 `vkprintf_impl` 函数模板，只是传入一个 mock 的 OutputFn——一个往 `std::string` 里追加字符的 lambda。这样测试覆盖的就是实际运行的代码，不存在不同步的问题。

**实现约束**：测试文件需要定义一个辅助函数 `do_printf`，它接受格式字符串和可变参数，内部创建一个空的 `std::string`，然后用 `vkprintf_impl` 格式化到一个往这个字符串追加字符的 lambda 里，最后返回这个字符串。这样测试断言就可以写成非常清晰的形式——比较 `do_printf("%d", 42)` 的返回值是否等于 `"42"`。

测试用例需要覆盖以下场景：基本的 `%d`、`%u`、`%x`、`%X`、`%s`、`%c`、`%p`、`%%`；宽度修饰符 `%10d`（右对齐空格填充）、`%010d`（右对齐零填充）；左对齐 `%-10d`、`%-20s`；负数零填充 `%06d` 配合 -42 得到 `-00042`；混合格式说明符的字符串；边界值（0、INT_MAX、INT_MIN、0x0 指针）；长度修饰符 `%ld`、`%lu`、`%llx`。

**踩坑预警**：在 host 端编译内核代码时要注意编译器的差异。`vkprintf_impl.hpp` 使用了 `va_list` 和 `va_start`/`va_end`，这些在 host 端的 g++ 下是可用的，但如果你不小心 include 了任何内核特有的头文件（比如 `io.hpp`），编译会直接失败——因为 host 端没有 `io_outb` 这些函数。确保测试文件只 include `vkprintf_impl.hpp` 和标准库头文件。

另一个容易忽略的问题是 `int64_t` 在 host 端和内核端的定义可能不同——在 x86_64 LP64 ABI 上，`long` 和 `long long` 都是 64 位的，所以 `%ld` 和 `%lld` 的行为在 host 端和内核端应该一致。但如果将来移植到 32 位目标，`long` 变成 32 位，这些格式说明符的行为就会不一样。目前 Cinux 只支持 x86_64，所以这不是问题。

**验证**：在 host 端直接运行单元测试，不需要 QEMU。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
# 预期：所有 kprintf 相关的测试用例全部通过
# 包括左对齐、零填充+负数、基本格式说明符、边界值等
```

### Step 4: 精简 kprintf.cpp——用共享引擎替换旧代码

**目标**：把 `kernel/lib/kprintf.cpp` 里的旧格式化代码（`format_decimal`、`format_hex`、`vkprintf` 实现）全部删除，替换为 include `vkprintf_impl.hpp`，只保留 Serial 全局对象管理和格式化调用的转发代码。

**设计思路**：重构后的 `kprintf.cpp` 应该非常精简——它只做三件事：管理全局 Serial 单例、初始化 Serial 并通过 `kprintf_init` 暴露、把 kprintf/kvprintf/kpanic 的调用转发给 `vkprintf_impl`。所有格式化的逻辑都交给共享头文件，`kprintf.cpp` 不再包含任何格式化相关的代码。

**实现约束**：`kprintf.cpp` 需要包含 `kernel/lib/private/vkprintf_impl.hpp` 和 `kernel/drivers/serial.hpp`。匿名命名空间中包含全局 `static Serial g_serial(SERIAL_COM1)` 和 `using cinux::lib::detail::vkprintf_impl` 声明。公开的 `kprintf_init` 调用 `g_serial.init()`。`kprintf` 和 `kvprintf` 各自调用 `vkprintf_impl`，传入一个捕获 `g_serial` 引用的 lambda。`kpanic` 在格式化输出后进入 `cli; hlt` 死循环。

小内核那边也需要做同样的精简——`kernel/mini/lib/kprintf.cpp` 删除旧代码，include `kernel/mini/lib/private/vkprintf_impl.h`（注意小内核用的是 `.h` 后缀，因为小内核的版本可能做了一些 C 兼容的调整，比如不用 C++ 命名空间）。

**踩坑预警**：删代码永远比写代码容易出问题。精简的时候一定要确保 include 路径正确——`private/vkprintf_impl.hpp` 的路径必须被 CMake 的 include 目录列表覆盖。另外，删除旧代码之后如果编译报"undefined reference to format_decimal"之类的错误，说明某个地方还在引用旧函数而不是使用共享头文件里的新函数。用 grep 搜一下整个项目确保没有残留的旧格式化函数定义。

**验证**：精简后重新构建并运行，所有串口输出应该和之前完全一致。同时运行单元测试确认格式化功能没有回退。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure && make run
# 预期：单元测试全部通过，串口输出和精简前完全一致
```

### Step 5: kernel_main 里加回归测试

**目标**：在 `kernel_main` 里添加几行格式化回归测试，用 kprintf 打印各种格式说明符的组合，确认引擎在实际硬件环境（QEMU）下的输出是正确的。

**设计思路**：单元测试在 host 端跑，覆盖的是格式化逻辑本身。但在实际的 QEMU 环境里，va_list 的 ABI、Serial 输出的字符正确性等因素都可能影响最终结果。所以加几行回归测试是有意义的——不是为了替代单元测试，而是作为端到端的冒烟测试。

**实现约束**：在 kernel_main 的初始化完成之后、进入 idle 循环之前，加几行 kprintf 调用，覆盖 `%d`、`%-10d`、`%06d` 配合负数、`%-20s`、`%p` 等格式。这些输出会直接显示在串口终端上，你可以目视确认格式是否正确。

**验证**：运行内核，目视检查串口输出中的格式化结果。

```bash
cmake --build build -j$(nproc)
cd build && make run
# 预期：在初始化日志之后，你能看到几行格式化测试输出
# 检查左对齐是否右边有空格、右对齐是否左边有空格/零
# 负数零填充是否是 -00042 而不是 000-42
```

---

## 构建与运行

```bash
# 切换到 tag
git checkout 012_driver_serial

# 配置 + 构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)

# 先跑单元测试（不需要 QEMU）
cd build && ctest --output-on-failure

# 再跑内核（需要 QEMU）
cd build && make run
```

单元测试应该覆盖所有格式说明符的基本功能和边界情况。内核运行时串口输出应该和重构前完全一致——任何差异都意味着重构引入了 bug。

---

## 调试技巧

**格式化输出和预期不一致**

最直接的排查方法是在单元测试里加一个失败的断言——比如 `ASSERT_EQ(do_printf("%-10d", 42), "42        ")`（42 后面跟 8 个空格）。如果断言失败，测试框架会打印期望值和实际值，你可以直接对比。对于只在 QEMU 环境下才出现的问题（host 端测试通过但内核输出不对），检查 va_list 的传递是否正确——在 x86_64 的 ABI 下 `va_list` 是一个数组类型，传参时要注意。

**提取后编译报 undefined reference**

这说明某个编译单元还在引用旧版本的格式化函数，而不是使用共享头文件里的新版本。用 grep 搜索整个项目里所有 `format_decimal`、`format_hex` 的定义和声明，确保只存在于 `vkprintf_impl.hpp` 和小内核的 `vkprintf_impl.h` 中。`kprintf.cpp` 和 `mini/lib/kprintf.cpp` 不应该有这些函数的定义。

**左对齐的 padding 是零而不是空格**

检查格式化引擎里左对齐分支的 padding 字符——它应该硬编码为空格字符 `' '`，而不是根据 `zero_pad` 标志来选择。左对齐模式下零填充标志应该被忽略。

---

## 本章小结

| 组件 | 文件路径 | 说明 |
|------|---------|------|
| 共享格式化引擎 | `kernel/lib/private/vkprintf_impl.hpp` | 模板化 OutputFn，硬件无关 |
| 小内核版本 | `kernel/mini/lib/private/vkprintf_impl.h` | C 兼容适配，同样的格式化逻辑 |
| 左对齐标志 | `%-Nd`、`%-Ns` | `-` 标志先输出内容再补空格 |
| 负数零填充 | `%06d` 配合 -42 = `-00042` | 先输出符号再补零再输出数字 |
| MockOutput 测试 | `tests/unit/test_kprintf.cpp` | 直接使用真正的 vkprintf_impl |
| DRY 重构 | 大/小内核共享同一份格式化代码 | 新增格式说明符只需改一个文件 |
| lambda 集成 | `[&](char c) { g_serial.putc(c); }` | 格式化引擎通过回调与 Serial 解耦 |
