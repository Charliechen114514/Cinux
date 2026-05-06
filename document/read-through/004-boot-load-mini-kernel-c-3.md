# 004 通读版 · 全局构造函数、C++ 运行时与特性验证

## 概览

在上一篇文章里，我们看完了 `boot.S` 的入口序列——从 `cli` 关中断到 BSS 清除，经历了 `rep stosb` 破坏 `%rdi` 和符号地址冲突两个经典 bug。BSS 清除之后，内核的内存状态终于干净了，接下来要做的是调用全局对象的构造函数，然后进入 C++ 的 `main` 函数。

本文覆盖 004C 内核侧的后半部分：全局构造函数调用机制（`.init_array` 遍历）、C++ freestanding 运行时支撑函数（`crt_stub.cpp`）、内核主函数和 C++ 特性验证（`main.cpp`），以及构建配置（`CMakeLists.txt`）。在讲解过程中，我们会看到 `crt_stub.cpp` 中的运行时 stub 如何让编译器生成的 C++ 代码在没有任何标准库的环境下正常工作。

关键设计决策一览：`.init_array` 的全局构造函数由内核自身遍历调用（ARM BPABI 方式）；`operator new/delete` 采用 halt 实现，在没有 PMM 阶段直接暴露编程错误；编译选项 `-mcmodel=large` 和 `-mno-red-zone` 是高半核内核的硬性要求。

## 代码精讲

### 全局构造函数调用——C++ 的隐藏基础设施

BSS 清除完成后，内核的内存状态终于干净了——所有未初始化的全局变量都被清零。接下来要做的是调用全局对象的构造函数。

```asm
    call _init_global_ctors

    movb $0x34, %al
    outb %al, $0xE9
```

`_init_global_ctors` 定义在 `crt_stub.cpp` 中。在解释它的实现之前，我们先理解为什么需要手动调用全局构造函数。

在有标准 C++ 运行时的环境中（比如 Linux 用户态），`crt0.o`（C Runtime Startup）会在调用 `main` 之前自动遍历 `.init_array` 段，调用所有注册的初始化函数。但我们用的是 `-nostdlib -ffreestanding`，没有任何 C 运行时代码——`_start` 是程序真正的入口，没有人帮我们做这件事。如果不去调用全局构造函数，`GlobalCounter global_counter` 这个全局对象的构造函数永远不会执行，`global_construction_count` 永远是 0，所有依赖全局初始化的代码都会出错。

### crt_stub.cpp——freestanding 环境下的 C++ 运行时支撑

`kernel/mini/arch/x86_64/crt_stub.cpp` 提供了 C++ 在 freestanding 环境下必须存在的一组运行时函数。这些函数在有 libstdc++ 的环境中由标准库提供，但在内核开发中必须手动实现。

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`__cxa_pure_virtual` 在通过虚函数表调用纯虚函数时被调用——这永远是一个编程错误。在用户态程序中它通常会打印错误信息然后 `abort()`，在内核中我们只能停机。即使没有任何代码直接调用它，vtable 的生成也会隐式引用它，所以链接器要求它必须存在。

```cpp
[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

`__stack_chk_fail` 是栈保护（Stack Canary）检测到溢出时的处理函数。如果编译时启用了 `-fstack-protector`，编译器会在函数栈帧中插入一个随机 canary 值，函数返回前检查 canary 是否被改写。改写意味着发生了栈溢出（通常是缓冲区越界写入），此时 `__stack_chk_fail` 被调用。

`__cxa_atexit` 是 `atexit` 的底层实现，用于注册程序退出时的清理函数。在内核中我们不支持"程序退出"这个概念——内核是一个永不终止的循环，所以这个函数直接返回 0（假装注册成功）。有些编译器生成的析构代码会引用它，即使析构函数永远不会被调用，链接器也需要这个符号存在。

接下来是核心函数——全局构造函数初始化：

```cpp
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)()   = __init_array_end;

    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) {
            ctor();
        }
    }
}
```

`__init_array_start` 和 `__init_array_end` 是链接器脚本中定义的符号，标记了 `.init_array` 段的起止。`.init_array` 段的内容是一个函数指针数组——GCC 为每个有非平凡构造函数的全局对象生成一个名为 `_GLOBAL__sub_I_XXX` 的函数，这个函数负责调用该对象的构造函数，然后把函数指针放在 `.init_array` 段中。

遍历方式很直接：从 `__init_array_start` 开始，逐个调用函数指针，直到 `__init_array_end`。这里有一个细节值得注意——代码在调用之前检查了 `nullptr`，这是因为某些链接器配置下 `.init_array` 的末尾可能存在空指针填充（特别是启用了 section sorting 时），跳过空指针可以避免意外跳到地址 0 触发 Triple Fault。

这是 OSDev Wiki 上推荐的 "ARM BPABI method"——简单、可靠、不依赖 `.init` 段的级联调用链。Linux 内核也使用几乎相同的机制（`do_initcalls`），只不过它分了多个 level（`__initcall_start` 到 `__initcall_end`，按优先级排列）。

然后是 `operator new/delete`：

```cpp
}  // extern "C" ends here

void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void operator delete(void* ptr, unsigned long size) noexcept {
    (void)ptr;
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void* operator new[](unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这些操作符的 halt 实现看起来很激进——调用 `new` 或 `delete` 直接停机。但在当前阶段这是合理的：内核还没有物理内存管理器（PMM），没有任何动态内存分配的基础设施。如果代码试图 `new` 一个对象，那一定是个 bug，停机是最好的选择——比起返回 `nullptr` 然后在某个遥远的地方触发空指针解引用，直接 halt 能让你立刻定位问题所在。

注意几个重要细节：`operator delete` 需要提供两个重载——一个是 C++11 的 `operator delete(void*)`，另一个是 C++14 引入的 sized deallocation 版本 `operator delete(void*, unsigned long)`。如果只提供一个，链接器会报 missing symbol 错误。同理，`operator new[]`（数组 new）也必须提供，否则使用数组的代码会链接失败。

这些 `new/delete` 运算符函数必须放在 `extern "C"` 块之外——它们需要 C++ 的 name mangling，因为 `operator new` 和 `operator delete` 是通过 C++ 的重载解析机制来匹配的。如果你把它们包在 `extern "C"` 里，链接器会找不到正确的 mangled 名称，报 missing symbol 错误。而上面那几个 `__cxa_*` 和 `__stack_chk_fail` 函数则需要 `extern "C"` 包裹，因为它们是被编译器按 C 链接约定调用的。`_init_global_ctors` 同样需要 `extern "C"`，因为它在汇编中被 `call` 调用，汇编代码只能用 C 符号名。

### 内核主函数——C++ 特性验证

`kernel/mini/main.cpp` 是内核的 C++ 主函数。在当前 tag 中，它的主要职责不是做什么有用的工作，而是验证 C++ 的各种特性在 freestanding 环境下是否正常工作。

```cpp
extern "C" {
extern uint64_t __boot_info_ptr;
}

static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}
```

`__boot_info_ptr` 声明为 `uint64_t`（不是指针类型），因为它是 `boot.S` 中 `.data` 段的一个 8 字节变量，里面存的是 BootInfo 的地址值。把它声明为 `extern uint64_t` 然后强制转换为 `BootInfo*` 是最直接的做法。`debugcon_putc` 是一个内联汇编辅助函数，往 debugcon 端口（0xE9）输出一个字符。

第一个测试是简单的类构造和析构，验证最基本的 C++ 特性——成员初始化列表、构造函数调用、成员函数调用都能正常工作。构造时输出 `'C'` 加上 value 的数字（比如 `C1`）。如果 SSE 没有正确启用（boot.S 中设置了 CR4.OSFXSR 和 CR4.OSXMMEXCPT），或者栈没有对齐，构造函数就会在访问成员变量时触发 General Protection Fault。

第二个测试是虚函数和多态——`Base` 是一个抽象类（有两个纯虚函数），`Derived` 继承并实现了它们。通过基类指针调用虚函数需要 vtable 的间接寻址。如果链接器没有正确生成 vtable（比如缺少 `__cxa_pure_virtual` 的定义），或者 `.rodata` 段的地址映射有问题，这里就会崩溃。输出 `'V'` 表示 Derived 的构造函数被调用。

第三个测试是全局对象构造——`global_counter` 是一个全局对象，它的构造函数不是由 `mini_kernel_main` 中的代码显式调用的，而是由 `_init_global_ctors` 遍历 `.init_array` 时间接调用的。GCC 会生成一个 `_GLOBAL__sub_I_global_counter` 函数放在 `.init_array` 中，这个函数调用 `GlobalCounter::GlobalCounter()`。这就是我们在上一篇符号冲突故事中提到的那个全局对象——正是它的 `global_construction_count = 42` 导致了 `__boot_info_ptr` 被覆盖为 `0x2a00000000`。

最后看 `mini_kernel_main` 的主体：

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('C');
    debugcon_putc('P');
    debugcon_putc('P');

    SimpleClass obj1(1);
    if (obj1.getValue() == 1 && obj1.getMarker() == 'S') {
        debugcon_putc('1');
    }

    Derived derived(5);
    Base* base = &derived;
    if (base->getName() == 'D' && base->compute() == 10) {
        debugcon_putc('2');
    }

    if (global_counter.getCount() == 42) {
        debugcon_putc('3');
    }

    if (boot_info->entry_point == 0xFFFFFFFF80020000
        && boot_info->kernel_phys_base == 0x20000) {
        debugcon_putc('B');
    }

    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('E');
    debugcon_putc('N');
    debugcon_putc('D');

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这里有一个值得注意的设计选择：`mini_kernel_main` 接收 `boot_info_addr` 参数（来自 `%rdi`），但实际使用的是 `__boot_info_ptr`（来自 `.data` 段）。`boot_info_addr` 被 `(void)` 显式忽略。这样做的原因是使用 `.data` 段中保存的值更可靠——它不受 System V ABI 调用约定的 volatile 寄存器规则影响，也不会被 BSS 清除或符号冲突破坏（修复之后）。

BootInfo 验证检查了两个字段：`entry_point` 应该是 `0xFFFFFFFF80020000`，`kernel_phys_base` 应该是 `0x20000`。如果这两个值正确，说明 BootInfo 从 bootloader 到内核的传递链条是完整的——bootloader 在 `stage2.S` 中填充、通过 `%rdi` 传递、`boot.S` 保存到 `.data` 段、`main.cpp` 读回来验证。

### 构建配置

`kernel/mini/CMakeLists.txt` 定义了小内核的构建规则。编译选项每一个都有存在的理由：

```cmake
set(MINI_KERNEL_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)
```

`-ffreestanding` 告诉编译器这是 freestanding 环境（没有标准库），禁止隐式引用标准库函数。`-fno-exceptions` 和 `-fno-rtti` 禁用异常和运行时类型信息——这两者在内核中既不必要也有性能开销。`-fno-pie` 禁用位置无关可执行文件——内核加载地址是固定的，不需要 PIC 重定位。`-mcmodel=large` 允许代码访问任意虚拟地址——默认的 `small` 内存模型假设所有符号都在低 2GB 地址空间内，但我们的内核在高半核地址，超出这个范围。`-mno-red-zone` 禁用 x86-64 的 Red Zone 优化——内核代码随时可能被中断打断，Red Zone（栈指针以下 128 字节不可触碰区域）在中断上下文中不安全。

链接后用 `objcopy -O binary` 把 ELF 转成扁平二进制——这个命令剥离所有 ELF header 和 section header，只保留各段的原始内容，按地址顺序输出。bootloader 直接把 flat binary 加载到物理地址 0x20000 后，代码的相对偏移就是正确的。

注意链接脚本中 `.text` 段使用了 `.text.start` 子段来确保 `_start` 在输出文件的最前面：`*(.text.start)` 放在 `*(.text .text.*)` 之前。boot.S 中把 `_start` 放进了 `.section .text.start, "ax"` 段，这样链接器会首先排放这个段，保证 flat binary 的第一个字节就是 `_start` 的入口代码。

## 设计决策

### 决策：手动遍历 `.init_array` vs 使用 `.init` 段级联

**问题**：全局对象的构造函数应该通过什么机制调用？

**本项目的做法**：直接遍历 `.init_array` 段的函数指针数组，从 `__init_array_start` 到 `__init_array_end`，逐个调用。这是 OSDev Wiki 上称为 "ARM BPABI method" 的方式。

**备选方案**：传统的 GNU 工具链使用 `.init` 段的级联调用：`crti.o` 提供 `.init` 段的 prologue，`crtbegin.o` 把 `.init_array` 的调用代码插入 `.init` 段，`crtend.o` 提供调用代码的尾部，`crtn.o` 提供 epilogue。链接时这些片段按顺序合并，形成完整的初始化函数。

**为什么不选备选方案**：级联调用需要提供 `crti.o` 和 `crtn.o` 的自定义版本（标准的来自 glibc，不能直接用），增加了构建系统的复杂度。直接遍历 `.init_array` 只需要几行代码，没有任何外部依赖。两种方式的效果完全相同——都是按顺序调用 `.init_array` 中的函数指针。

**如果要扩展/改进，应该怎么做**：如果后续需要支持构造优先级（比如某些全局对象必须在其他对象之前初始化），可以像 Linux 内核一样定义多个 level（`initcall0`、`initcall1` 等），每个 level 是 `.init_array` 的一个子区间，按 level 顺序调用。

### 决策：`new`/`delete` halt vs 返回错误

**问题**：在还没有内存管理器的阶段，`operator new` 应该怎么实现？

**本项目的做法**：halt（死循环）。调用 `new` 就意味着遇到了编程错误，直接停机。

**备选方案**：返回 `nullptr`。调用者应该检查返回值并做错误处理——但 C++ 的 `new` 默认不应该返回 `nullptr`（那是 `nothrow new` 的行为），返回 `nullptr` 会导致未定义行为。

**为什么不选备选方案**：在内核环境中，如果代码试图动态分配内存但分配器还没准备好，这是一个设计错误而不是运行时错误。停机可以立即暴露问题，而返回 `nullptr` 可能导致后续的空指针解引用，报错位置和实际出错位置相差很远，更难调试。

**如果要扩展/改进，应该怎么做**：当 PMM 和堆分配器上线后，替换这些 stub 为真正的内存分配实现。`operator new` 调用内核的 `kmalloc()`，`operator delete` 调用 `kfree()`。

## 扩展方向

1. **添加析构函数测试**（难度：简单）——当前 `mini_kernel_main` 在 `while(1) hlt` 中结束，局部对象的析构函数永远不会执行。可以在 halt 前手动调用析构函数（或者用 `placement new` 在指定地址构造对象），验证析构函数链的正确性。

2. **实现简单的堆分配器**（难度：中等）——替换 `operator new` 和 `operator delete` 的 halt 实现为基于 BSS 末尾的 bump allocator（只分配不释放）。这样可以验证 `new`/`delete` 在 freestanding 环境下的完整工作流。

3. **使用 `__attribute__((constructor))` 指定优先级**（难度：简单）——在 `crt_stub.cpp` 中添加一个带 `__attribute__((constructor(101)))` 的函数，验证 `.init_array` 的优先级排序是否按预期工作（数值小的先执行）。GCC 会把不同优先级的构造函数放在 `.init_array` 的不同子段中。

4. **内核命令行解析**（难度：中等）——在 BootInfo 中添加 `cmdline` 字段，bootloader 从固定位置读取内核命令行字符串，内核解析后控制行为（比如 `debug=verbose` 开启详细日志）。

## 参考资料

- OSDev Wiki -- Calling Global Constructors: `.init_array` 段的遍历方法详解，包括 ARM BPABI 风格（直接遍历函数指针数组）和传统 GNU 风格（`crti.o`/`crtbegin.o` 级联）。Cinux 使用前者。
  https://wiki.osdev.org/Calling_Global_Constructors

- OSDev Wiki -- C++ Bare Bones: freestanding 环境下必须提供的运行时函数清单（`__cxa_pure_virtual`、`__cxa_atexit`、`operator new/delete`），以及 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti` 编译选项的说明。
  https://wiki.osdev.org/C%2B%2B_Bare_Bones

- OSDev Wiki -- Multiboot Specification: GRUB 为 ELF 内核清除 BSS 的保证。Cinux 不使用 GRUB，所以必须手动清零——这也是触发 BSS 清除 bug 的根本原因。
  https://wiki.osdev.org/Multiboot

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口序列，对比 Cinux 的 `boot.S`。Linux 把 boot_params 保存到 `%r15`（callee-saved），依赖 decompressor 清零 BSS，使用 `__START_KERNEL_map` 作为高半核基址。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S

- xv6 `entry.S` (MIT): xv6 的入口代码——比 Cinux 简单得多，因为 xv6 依赖 GRUB（Multiboot）完成 BSS 清零，且是纯 C（没有 `.init_array` 和全局构造函数）。
  https://github.com/mit-pdos/xv6-public/blob/master/entry.S
