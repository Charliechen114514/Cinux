# SSE 初始化修复与测试框架走读

## 概览

这篇文章覆盖 tag 012 中两个看似不相关但实际上紧密联系的修改。第一个是迷你内核 `boot.S` 中添加的 SSE 初始化代码——这是一个 -O2 构建崩溃的 bug fix，崩溃链条从 SSE 指令触发 #UD，到 IDT 未加载导致 Triple Fault，再到 QEMU 静默退出，整个过程堪称教科书级别的"蝴蝶效应"。第二个是 `tests/unit/test_kprintf.cpp` 的彻底重写——受益于格式化引擎重构，测试从"复制品测试"升级为直接测试生产代码。我们还会看看 `kernel_main.cpp` 中新增的格式化回归测试。

## 架构图

```
boot.S SSE 初始化流程:

  _start
    |
    +-- cli                    禁中断
    |
    +-- CR4.OSFXSR |= (1<<9)  允许 SSE 指令执行
    +-- CR4.OSXMMEXCPT |= (1<<10) 允许 SIMD 浮点异常
    +-- movq %rax, %cr4        写回 CR4
    +-- clts                    清除 CR0.TS
    |
    +-- debugcon '1'           标记：SSE 初始化完成
    +-- 设置栈指针
    +-- 保存 BootInfo
    +-- 清零 BSS
    +-- 调用全局构造函数
    +-- 调用 mini_kernel_main


测试架构:

  test_kprintf.cpp (host-side, -DCINUX_HOST_TEST)
    |
    +-- #include "vkprintf_impl.hpp"   直接引用生产引擎
    |
    +-- MockOutput                     std::string 缓冲区
    +-- do_printf(fmt, ...)            va_list -> vkprintf_impl -> MockOutput
    |
    +-- 35 个 TEST() 用例
         |
         +-- %d / %u / %x / %X / %s / %p / %c / %%
         +-- 宽度: %08x, %4d
         +-- 左对齐: %-10d, %-10s
         +-- 负数零填充: %06d with -42
         +-- 边界情况: nullptr, 空字符串, 空格式串
```

## 代码精讲

### boot.S 中的 SSE 初始化

先来看 `kernel/mini/arch/x86_64/boot.S` 的关键修改部分。

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    cli

    /* Enable SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: enable SIMD #XF */
    movq %rax, %cr4
    clts                          /* Clear CR0.TS (Task Switched) */
```

这段代码紧跟在 `cli` 之后、栈设置之前执行，是内核最早接触的几条指令之一。我们逐行来拆解。

`cli` 禁用中断，这是内核入口的标准做法，避免在初始化还没完成时被中断打断。

接下来的三行操作 CR4 寄存器。`movq %cr4, %rax` 把 CR4 的当前值读到 RAX，然后用 `orq` 设置 bit 9 和 bit 10。bit 9 是 **OSFXSR**——操作系统通过设置这个位来告知 CPU "我支持 FXSAVE/FXRSTOR 指令并且准备好了管理 SSE/MMX 状态"。如果这个位是 0，CPU 在执行 SSE 指令（比如 `pxor %xmm0, %xmm0`）时会触发 #UD（Invalid Opcode）。bit 10 是 **OSXMMEXCPT**——设置后，SIMD 浮点异常会通过 #XM（向量 19）传递，而不是触发 #UD。最后 `movq %rax, %cr4` 把修改后的值写回 CR4。

`clts` 指令清除 CR0 的 TS（Task Switched）位。TS 位在 CPU 做任务切换时会被自动设置，此时执行 FPU/SSE 指令会触发 #NM（Device Not Available）。虽然在我们这个场景中，从 bootloader 跳转到内核时 TS 不一定是 1，但显式清除它是一个好的防御性编程习惯——不依赖上层的固件给你留一个干净的 CR0。

这几行代码解决了什么问题？答案要从一次 -O2 构建崩溃说起。

### 崩溃的故事

事情是这样的：当用 `CMAKE_BUILD_TYPE=Release`（`-O2` 优化）构建迷你内核时，内核在 IDT 初始化阶段悄无声息地 Triple Fault 崩溃了，而 Debug 模式（`-O0`）一切正常。QEMU 的退出码是 0——不是 `isa-debug-exit` 设备产生的奇数退出码，说明内核根本没有执行到测试框架的退出逻辑，而是直接 Triple Fault 后被 `-no-reboot` 标志终止了。

崩溃只发生在 "Setting up IDT..." 之后的输出全部消失这一刻。通过在 `idt_init()` 的各个步骤间插入 `outb` 到 debugcon（端口 0xE9）作为标记，可以精确定位到 IDT 清零循环。

`-O2` 和 `-O0` 的区别在哪里？编译器在 `-O2` 模式下，将结构体清零循环向量化为 SSE 指令：

```asm
; -O2 生成的 idt_init() 清零循环
ffffffff800239e3:   pxor   %xmm0,%xmm0          ; 第一条 SSE 指令，就在这里崩溃
ffffffff800239ee:   movaps %xmm0,(%rcx,%rdx,1)   ; 16 字节对齐写入
```

`pxor %xmm0, %xmm0` 是一条 SSE 指令——128 位异或操作，编译器用它来高效地将内存区域清零（一次写 16 字节）。但问题是，此时 CR4.OSFXSR 还是 0（bootloader 的 `enter_long_mode` 只设了 CR4.PAE 来启用长模式分页，从未设过 OSFXSR），CPU 看到 SSE 指令就触发 #UD。更要命的是，IDT 还没加载（limit=0），所以 #UD 找不到处理程序，变成了 Triple Fault，QEMU 直接退出。

这里有一个很容易踩的认知陷阱：很多人（包括写这段代码之前的我）以为 64 位长模式下 SSE 指令是一定能执行的。事实上，AMD64 架构确实要求 64 位 CPU 硬件上支持 SSE，但 CPU 仍然会检查 CR4.OSFXSR 这个控制位。硬件支持不等于软件允许——操作系统必须显式地"开门"，SSE 指令才能真正执行。Intel SDM Vol. 3A Section 2.5 明确说明了这一点。

```asm
    /* Output '1' to debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9

    /* Setup stack - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp
```

SSE 初始化之后的代码和之前一样——向 debugcon 输出标记字符 '1' 表示入口已到达，然后设置栈指针。SSE 初始化放在最早的位置是因为编译器在 `-O2` 下可能在任何函数中使用 SSE 指令（不只是 `idt_init` 的清零循环），所以必须在所有 C++ 代码执行之前完成初始化。

### debugcon 标记调试法

在整个排查过程中，`outb` 到端口 0xE9 这个技巧帮了大忙。QEMU 的 debugcon 设备会把写入端口 0xE9 的字节输出到日志文件（通过 `-debugcon stdio` 或 `-debugcon file:debug.log` 参数）。这个方法的优势在于它只使用一条 `outb` 指令，不依赖任何内核基础设施——不需要串口初始化、不需要 GDT、不需要 IDT、甚至不需要栈。只要 CPU 能执行指令，debugcon 就能工作。

排查时，在 `idt_init()` 的各个关键步骤前插入标记字符：

```
debug.log 输出: OPLJ1234...0
```

字符 '0' 之后再也没有其他标记——说明崩溃发生在 '0' 标记对应的那个步骤之后，精确定位到了 IDT 清零循环中的 SSE 指令。

### 测试框架重写 —— MockOutput 与 do_printf

现在我们来看测试代码的完整架构。

```cpp
// tests/unit/test_kprintf.cpp

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <string>
#include <stdarg.h>

// Include the big kernel formatting engine
#include "lib/private/vkprintf_impl.hpp"

using namespace cinux::lib::detail;
```

`TEST_FRAMEWORK_IMPL` 宏让 `test_framework.h` 在此编译单元中生成测试框架的实现代码（而不是只有声明）。`CINUX_HOST_TEST` 是 CMake 在编译测试可执行文件时定义的宏，用来区分主机端测试和内核端代码。`#include "lib/private/vkprintf_impl.hpp"` 直接把大内核的格式化引擎拉进来——这是重构的关键收益，之前这个 include 是不可能的，因为格式化引擎和串口驱动耦合在一起。

```cpp
class MockOutput {
public:
    void putc(char c) { buffer_.push_back(c); }
    std::string result() const { return buffer_; }
    void        clear() { buffer_.clear(); }

private:
    std::string buffer_;
};
```

`MockOutput` 类模拟了 `Serial` 的接口——只有一个 `putc` 方法，把字符追加到内部的 `std::string` 中。`result()` 返回累积的输出内容用于断言，`clear()` 清空缓冲区以便复用。这个类比之前的 `MockFormatter` 简单得多——不需要 `puts()` 方法，因为格式化引擎已经逐字符调用了。

```cpp
static std::string do_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    MockOutput mock;
    vkprintf_impl([&](char c) { mock.putc(c); }, fmt, args);

    va_end(args);
    return mock.result();
}
```

`do_printf` 是整个测试框架的"桥梁"——它接受 printf 风格的格式字符串和变参，内部构造一个 `MockOutput`，把 `vkprintf_impl` 的输出重定向到这个 mock 对象，然后返回结果字符串。这个函数让测试用例写起来极其自然：

```cpp
ASSERT_EQ(do_printf("%d", 42), "42");
```

和写 `printf` 几乎一样直观，但断言的是精确的输出内容。

### 测试用例分类走读

测试总共 35 个用例，按功能分组。

**%d 有符号十进制测试**覆盖了正数、负数、零、大正数和大负数，验证 `format_decimal` 的基本正确性和 INT64_MIN 的特殊处理路径。

**%u 无符号十进制测试**验证了小正数、大正数（接近 UINT32_MAX）和零。

**%x / %X 十六进制测试**覆盖了大小写转换、零、单数字、以及 16 位完整数字（`0x123456789ABCDEF0`）。

**%s 字符串测试**不仅测了正常字符串和空字符串，还专门测了 `nullptr` 安全（输出 `"(null)"`）、右对齐宽度（`[%10s]` with "hi" => `[        hi]`）、左对齐宽度（`[%-10s]` with "hi" => `[hi        ]`）、以及精确宽度（`[%5s]` with "hello" => `[hello]`）。

**%p 指针测试**验证了 "0x" 前缀 + 恰好 16 位十六进制数字的固定格式，包括零地址（`0x0000000000000000`）和最大 64 位值（`0xFFFFFFFFFFFFFFFF`）。

**宽度修饰符测试**覆盖了 `%08x`（零填充）、`%4d`（空格填充）、以及当实际数字已经达到或超过宽度时的行为（`%08x` with `0xDEADBEEF` => `"deadbeef"`，不会截断）。

**负数零填充测试**是这个 tag 新增功能的关键验证。`%06d` 格式化 `-42` 应该输出 `-00042`（负号在前、零填充在中间、数字在最后），而不是错误的 `000-42`。`%-6d` 格式化 `-42` 应该输出 `-42   `（左对齐，右侧补空格）。

```cpp
TEST("kprintf: %06d negative zero-pad") {
    ASSERT_EQ(do_printf("[%06d]", -42), "[-00042]");
}

TEST("kprintf: %-6d negative left-align") {
    ASSERT_EQ(do_printf("[%-6d]", -42), "[-42   ]");
}
```

**混合格式测试**验证多个格式说明符在同一格式字符串中的正确解析和输出，以及纯文本和边界情况（空格式串、尾部字面量、未知说明符回退到 `%q` 原样输出）。

### kernel_main 中的回归测试

最后我们来看 `kernel_main.cpp` 中的格式化回归测试。这些测试在真正的内核环境中运行：

```cpp
// kernel/main.cpp

// -- kprintf format regression test (after serial + GDT + IDT are up) --
cinux::lib::kprintf("[KPRINTF] %%d: %d\n", 42);
cinux::lib::kprintf("[KPRINTF] %%d negative: %d\n", -123);
cinux::lib::kprintf("[KPRINTF] %%u: %u\n", 4294967295u);
cinux::lib::kprintf("[KPRINTF] %%x: %x\n", 0xDEADBEEFu);
cinux::lib::kprintf("[KPRINTF] %%X: %X\n", 0xDEADBEEFu);
cinux::lib::kprintf("[KPRINTF] %%08x: %08x\n", 0xDEADu);
cinux::lib::kprintf("[KPRINTF] %%10d: %10d\n", 42);
cinux::lib::kprintf("[KPRINTF] %%-10d: %-10d|\n", 42);
cinux::lib::kprintf("[KPRINTF] %%s: %s\n", "hello");
cinux::lib::kprintf("[KPRINTF] %%-10s: %-10s|\n", "hi");
cinux::lib::kprintf("[KPRINTF] %%p: %p\n", (void*)0x1234ABCD5678ull);
cinux::lib::kprintf("[KPRINTF] %%c: %c\n", 'Z');
cinux::lib::kprintf("[KPRINTF] %%%%: %%\n");
cinux::lib::kprintf("[KPRINTF] %%010u: %010u\n", 42u);
cinux::lib::kprintf("[KPRINTF] mix: %s n=%d hex=%08x ptr=%p\n",
                    "test", 99, 0xCAFEBABEu, (void*)0x1ull);
cinux::lib::kprintf("[KPRINTF] all format tests done.\n");
```

这些测试放在 IDT 加载之后、PIC 初始化之前——因为此时串口已经初始化好了（kprintf_init 在 step 1 完成），GDT 和 IDT 也已经加载，可以安全地使用 kprintf 输出。每行测试覆盖一个格式说明符或宽度修饰符组合，尾部的 `|` 字符用来标记实际输出范围的边界，方便在串口日志中人工检查对齐效果。

这组回归测试的价值在于它们验证的是端到端的通路——从 `vkprintf_impl` 的格式解析，到 `Serial::putc` 的硬件轮询，到 `io_outb` 的端口写入，到 QEMU 虚拟 UART 的输出——整个链条。主机端的单元测试只验证格式化逻辑本身，而内核端的回归测试验证的是"逻辑 + 硬件 + 仿真器"的组合。两种测试互补，缺一不可。

## 设计决策

### Decision: SSE 初始化放在 boot.S vs long_mode.S

**问题**: SSE 控制位设置应该在哪个阶段完成？

**本项目的做法**: 在 `kernel/mini/arch/x86_64/boot.S` 的 `_start` 入口处、`cli` 之后立即执行。

**备选方案**: 在 bootloader 的 `long_mode.S` 中设置（因为那是从保护模式切换到长模式的地方，也是设置 CR4.PAE 的地方）。

**为什么不选备选方案**: boot.S 是内核的真正入口——所有后续内核代码都可能被编译器用 SSE 优化，而 boot.S 的 `_start` 是最早执行的内核代码。如果在 `long_mode.S` 中设置，那是 bootloader 的代码，而内核的 `-O2` 构建和 bootloader 是独立编译的——你不能假设 bootloader 已经替你设好了 CR4。把 SSE 初始化放在内核自己的入口处是最安全、最确定的做法。

### Decision: debugcon 标记法 vs GDB 调试

**问题**: 排查 Triple Fault 这种早期崩溃时，应该用什么调试手段？

**本项目的做法**: 在代码中插入 `outb` 到端口 0xE9 的标记字符，通过 QEMU debugcon 日志观察执行到了哪一步。

**备选方案**: 使用 GDB 远程调试（`qemu -s -S` + `gdb`），在崩溃点设断点、检查寄存器状态。

**为什么不选备选方案**: GDB 调试 Triple Fault 有一个鸡生蛋的问题——崩溃发生在 IDT 还没加载的时候，此时如果 GDB 的断点触发了异常，处理器找不到处理程序，会再次 Triple Fault。debugcon 标记法只需要一条 `outb` 指令，零依赖，可以在任何执行阶段使用，是排查早期引导问题的利器。当然，在定位到具体函数后，GDB 配合反汇编查看 CR0/CR4 寄存器值是很有价值的补充手段。

### Decision: 直接测试生产代码 vs 复制测试

**问题**: 单元测试应该测试生产格式化引擎还是复制品？

**本项目的做法**: 直接 `#include "vkprintf_impl.hpp"`，测试真正的格式化引擎。

**备选方案**: 之前的老方法——在测试文件里复制 `format_decimal`、`format_hex` 和格式解析逻辑，测试复制品。

**为什么不选备选方案**: 复制品测试的最大问题是，你测的是复制品而不是真正的引擎。如果引擎改了但忘了更新复制品（或者反过来），测试可能通过但生产代码有 bug。重构后格式化引擎是 header-only 的，测试可以直接 include，保证测试和生产的代码 100% 一致。这是这次重构带来的最重要的架构改进之一。

## 扩展方向

- **完整 CR0/CR4 初始化序列**: 当前只设了 OSFXSR、OSXMMEXCPT 和清除 TS。完整的 OS 引导 SSE 初始化还应包括清除 CR0.EM（bit 2）和设置 CR0.MP（bit 1）。可以在 boot.S 中补全这些步骤。（难度：⭐）

- **AVX 支持检测**: 现代编译器在更高优化级别下可能使用 AVX 指令（256 位 YMM 寄存器），这需要 CR4.OSXSAVE 和 XCR0 的额外配置。在内核入口添加 CPUID 检测和 AVX 使能代码。（难度：⭐⭐）

- **调试用 CR0/CR4 寄存器转储工具**: 在 boot.S 的 SSE 初始化前后把 CR0 和 CR4 的值输出到 debugcon，用二进制格式显示各控制位的状态。这个工具在排查类似问题时会非常有用。（难度：⭐）

- **自动化 -O0/-O2 回归对比测试**: 在 CI 中同时运行 Debug 和 Release 构建，对比测试结果。任何仅在 -O2 下出现的失败都可能是编译器向量化暴露出的控制位遗漏。（难度：⭐⭐）

- **Triple Fault 自动化诊断脚本**: 编写一个脚本，检测 QEMU 退出码为 0 的情况，自动提取 debugcon 日志，分析最后的标记字符位置，并给出可能的崩溃原因建议。（难度：⭐⭐）

## 参考资料

- Intel SDM: Vol. 3A Section 2.5 "Control Registers" — CR4.OSFXSR (bit 9) 必须 OS 设置以允许 FXSAVE/FXRSTOR 和 SSE 指令执行；CR4.OSXMMEXCPT (bit 10) 控制 SIMD 浮点异常传递方式；CR0.TS (bit 3) 在任务切换时设置，导致 FP/SSE 指令触发 #NM；CR0.EM (bit 2) 在 SSE 处理器上必须为 0
- Intel SDM: Vol. 2A Chapter 3 — IN/OUT 指令格式，8-bit 端口号作为立即数，64-bit 模式下操作数大小固定为 8 或 32 位
- OSDev Wiki: [SSE](https://wiki.osdev.org/SSE) — x86 SSE 初始化的完整指南，包括 CR0/CR4 设置和 FXSAVE 区域布局
