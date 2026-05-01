# 010-1 教程：大内核 GDT 初始化——为什么 long mode 还需要段描述符？

> 标签：x86_64, GDT, 段描述符, 长模式, TSS, lgdt, C++ scoped enum
> 前置：[009 大内核入口](./009-large-kernel-entry-1.md)

## 前言

说实话，当我第一次看到大内核在 QEMU 里 triple fault 的时候，整个人都是懵的——明明上一章还跑得好好的，串口输出正常、kprintf 格式化也没问题，怎么加了一行代码就直接重启了？后来才意识到，是因为 CPU 遇到了一个未处理的异常（具体是什么异常我根本不知道，因为没有任何输出），然后 IDT 是空的，然后 Double Fault，然后 Triple Fault，然后 QEMU 说再见。这就是没有 GDT/IDT 的内核的真实处境：你写的所有代码都可能在任何时刻被 CPU 异常杀死，而你对发生的事情一无所知。

这一章我们要给大内核装上完整的异常处理基础设施的第一块拼图——GDT（全局描述符表）。如果你跟着这个项目从头走过来的，你一定记得 002 章在 Bootloader 里配过一次 GDT，007 章在 mini kernel 里也配过一次。现在到了大内核，我们又要配第三次——这感觉确实有点荒谬，但实际上是必要的。Bootloader 的 GDT 在实模式切换保护模式时起作用，mini kernel 的 GDT 放在低地址区域，而大内核运行在 higher-half 虚拟地址空间，我们需要在自己管理的内存里重新建立 GDT，确保段寄存器和 IDT 中引用的段选择子都指向正确的位置。

完成本节后，大内核会拥有一个七项的 GDT：null、kernel code64、kernel data64、user code64、user data64、TSS low、TSS high。所有段寄存器都会刷新到正确的选择子，任务寄存器也会加载 TSS。这是下一节 IDT 和中断处理的绝对前置条件。

## 环境说明

我们在 WSL2 上开发，工具链是 GCC 13+ 配合 CMake，目标平台是 x86_64 freestanding。编译参数里有 `-mcmodel=kernel -mno-red-zone -ffreestanding` 这些内核专属选项。QEMU 用来运行和测试，串口输出通过 `-serial stdio` 转发到终端。

## 为什么 long mode 下还需要 GDT？

你可能会问一个很自然的问题：既然 64 位模式下分段机制基本被废了——Base 被硬件当作 0，Limit 被当作 `0xFFFFFFFFF`——那为什么我们还需要费劲配 GDT？Intel 的文档（SDM Vol.3A §3.4.5）确实说了 long mode 下"most segment descriptor fields are ignored"，但它紧接着列了一堆例外情况。

最关键的例外是 CS 寄存器。CPU 内部对 CS 的"隐藏部分"（段描述符缓存）做了大量检查——它根据 CS 缓存中的 L 位（Long mode）和 D 位（Default operation size）来决定当前是 64 位模式还是 32 位兼容模式。如果 CS 指向的描述符 L=0, D=1，CPU 就按 32 位模式执行代码，然后你的 `iretq` 指令直接变成非法操作码（32 位模式不支持 64 位的 iretq）。这种错误不会在"切换到 long mode"时立刻暴露——CS 可能还缓存着旧的描述符属性，直到下一次 far jump 或者 lgdt + lretq 强制刷新。

另一个关键场景是中断处理。IDT 里的每个条目都有一个 Segment Selector 字段（Intel SDM Vol.3A §6.14.1 Figure 6-8），指向 GDT 中的 code segment 描述符。CPU 在中断触发时会用这个选择子加载 CS。如果 GDT 里没有正确的 code64 描述符（Present=1, L=1, D=0, DPL=0），中断触发后 CPU 找不到合法的代码段，直接 #GP → Double Fault → Triple Fault → QEMU 重启。这就是我们之前遇到的"黑屏重启"的根本原因。

## 我们的 GDT 布局

现在我们来看大内核 GDT 的具体布局。这次我们的 GDT 比 mini kernel 的那次要"豪华"得多——不仅有内核态的 code 和 data 段，还预留了用户态的 code64 和 data64 段，外加一个 TSS（Task State Segment）占位符。

```
索引  选择子   内容               用途
────  ──────   ────               ────
0     0x00     Null Descriptor    Intel 规定第一项必须为空
1     0x08     Kernel Code64      内核代码段 (Ring 0, 64-bit)
2     0x10     Kernel Data64      内核数据段 (Ring 0)
3     0x1B     User Code64        用户代码段 (Ring 3, 64-bit)
4     0x23     User Data64        用户数据段 (Ring 3)
5-6   0x28     TSS (16 bytes)     任务状态段占位 (2 个 GDT slot)
```

选择子的值是 `索引 * 8 + RPL`。内核态 RPL=0，所以 Kernel Code64 = 1*8 = 0x08。用户态 RPL=3，所以 User Code64 = 3*8 + 3 = 0x1B，User Data64 = 4*8 + 3 = 0x23。TSS 虽然占了两个 slot，但选择子只指向索引 5，即 5*8 = 0x28。

用户态段（User Code64/Data64）目前不会被用到——我们还在纯内核态运行。但提前把它们放进 GDT 有两个好处：一是后续实现用户态（tag 022）时不需要回头修改 GDT 结构；二是很多 CPU 在做权限验证时会检查 GDT 中是否有完整的 Ring 0 + Ring 3 段描述符，缺失可能导致某些边界情况下的异常行为。

TSS 在 long mode 下的角色和 32 位模式完全不同。32 位时代 TSS 用于硬件任务切换——每个任务有自己的 TSS，CPU 通过 `jmp TSS_selector` 在任务间切换，自动保存/恢复寄存器。到了 64 位模式，硬件任务切换被彻底移除了（Intel SDM 明确说 "hardware task switching is not supported in 64-bit mode"）。TSS 的 64 位版本（104 字节，布局在 Intel SDM Vol.3A §7.7 Table 7-8）只剩下两个功能：提供 RSP0/RSP1/RSP2（权限级别切换时的栈指针）和 IST1-IST7（Interrupt Stack Table，为关键中断提供已知良好的栈）。我们的 TSS 目前是一个全零占位符——没有用户态代码不需要设置 RSP0，没有关键中断不需要设置 IST。但 GDT 里必须有这个条目，否则 `ltr` 指令触发 #GP。

### xv6 怎么做的

xv6 作为 MIT 的经典教学操作系统，它的 GDT 配置方式和我们很像，但也有一些有趣的差异。xv6 的 GDT 定义在 `mmu.h` 中，也是六项：null + KCODE + KDATA + UCODE + UDATA + TSS，选择子的值也完全相同（0x08, 0x10, 0x1B, 0x23, 0x28）。但 xv6 是 32 位系统，它的 GDT 条目只有 8 字节（不需要 64 位地址），TSS 也是 32 位版本（包含完整的寄存器保存区域）。xv6 在 `main.c` 的 `userinit()` 中设置 TSS 的 ss0 和 esp0 字段，然后 `ltr` 加载——比我们的全零占位符多了一步实际配置。

xv6 的 GDT 不是一个 class，而是一个全局数组 `struct segdesc gdt[NSEGS]`，通过 `SEG_NULL`、`SEG(STA_X|STA_R, 0, 0xffffffff)` 这样的宏来填充。这些宏展开后直接操作位域，可读性不如我们的 scoped enum 方案——比如 `SEG(STA_X|STA_R, 0, 0xffffffff)` 你得去查 `STA_X` 和 `STA_R` 的定义才知道这是"可执行可读的代码段"，而我们写 `SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite` 就一目了然了。不过 xv6 的宏方案更精简，一行就能定义一个描述符，对于小型教学系统来说可能更合适。

### Linux 怎么做的

Linux 的 GDT 方案就完全是另一个量级了。在 x86_64 Linux 中，每个 CPU 有自己独立的 GDT 页——`DECLARE_PER_CPU_PAGE_ALIGNED(struct gdt_page, gdt_page)`（定义在 `arch/x86/include/asm/desc.h`）。这意味着在 SMP 系统中，每个核的 GDT 互不干扰，AP（Application Processor）启动时会从 BSP（Bootstrap Processor）复制 GDT 内容然后独立管理。Cinux 目前是单核，用一个全局 `GDT g_gdt` 实例就够了，将来如果做 SMP，只需要改成 per-CPU 变量。

Linux 还有一个非常精巧的安全机制：GDT 被映射为只读页面（通过 fixmap 机制），需要修改时临时切换到可写副本。这个设计的动机是 `ltr` 指令会把 TSS 描述符的 type 从 "Available"（0x09）改成 "Busy"（0x0B），这涉及对 GDT 的写操作——如果 GDT 在只读页面上会触发页错误。Linux 的 `native_load_tr_desc()` 函数在执行 `ltr` 前先检查当前 GDT 是否是只读映射的 fixmap 地址，如果是就临时切换到可写副本，执行完 `ltr` 再切回只读。Cinux 的 GDT 在可写内存中，没有这种保护机制，但对于教学系统来说足够了。

Linux 的 `desc_struct` 使用位域来描述 GDT 条目（`base0`, `base1`, `base2`, `type`, `s`, `dpl`, `p`, `limit0`, `limit1` 等字段），编译器自动处理位域分配。我们的方案是用 `[[gnu::packed]]` 原始字节布局 + scoped enum 组合——Linux 的位域方案更接近 C 语言的惯用写法，我们的方案更直接地映射了 Intel SDM 的字节级描述。两种方案各有利弊：位域方案代码更紧凑，但依赖编译器的位域布局（虽然 GCC 在 `packed` struct 中的位域布局是确定的）；原始字节方案代码更冗长，但和硬件文档一一对应，调试时更直观。

## 第一步——类型安全的段描述符构建

我们现在要做的是创建 GDT 的 C++ 头文件。核心设计思路是用 scoped enum（`enum class`）来表达段描述符的 access byte 和 flags byte，配合重载的 `operator|` 来组合标志位。这种方案的好处是类型安全——你不可能意外地用一个 `SegmentFlags` 的值去组合 `SegmentAccess`，因为它们的类型不同，编译器会在编译期报错。

```cpp
namespace cinux::arch {

constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;

enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,
    Ring0      = 0u << 5,
    Ring3      = 3u << 5,
    CodeData   = 1u << 4,
    Executable = 1u << 3,
    ReadWrite  = 1u << 1,
    TSS64Avail = 0x09,
};

constexpr SegmentAccess operator|(SegmentAccess a, SegmentAccess b) {
    return static_cast<SegmentAccess>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

enum class SegmentFlags : uint8_t {
    Granularity4K = 1u << 3,
    LongMode      = 1u << 1,
    Size32        = 1u << 2,
};
```

你会发现 `SegmentAccess` 的枚举值直接对应 Intel SDM Vol.3A §3.4.5 Figure 3-8 中的 access byte 位域——`Present` 是 bit 7，`Ring0`/`Ring3` 是 bits 6:5（DPL），`CodeData` 是 bit 4（S 位），`Executable` 是 bit 3，`ReadWrite` 是 bit 1。每个值都是左移到正确位位置后的结果，所以组合时直接按位或就行。

`TSS64Avail = 0x09` 是一个特殊情况——它不是一个按 bit 组合的标志，而是一个完整的 access byte 值。0x09 = 0000_1001，bit 3 置 1 表示这是一个 64 位 TSS（Type 字段的 bit 1），bit 0 置 1 表示 "Available"（Type 字段的 bit 0，区别于 "Busy" 状态的 0x0B）。在 Intel SDM 中，TSS 的 access byte 是 `Present | TSS64Avail` = `0x80 | 0x09` = `0x89`。

`SegmentFlags` 的 `Size32` 值是 `1u << 2`（bit 2，对应 D/B 位），不是 bit 1。别和 `LongMode` 的 bit 1 搞混了。这里有一个很重要的细节——内核数据段的 flags 用的是 `Size32` 而不是 `LongMode`，原因是 64 位模式下只有代码段才需要设置 L 位，数据段的 L 位被 Intel 保留（必须为 0）。如果你把数据段的 L 位也置 1，CPU 行为是未定义的。

接下来是 GDT class 的内部结构体：

```cpp
class GDT {
public:
    void init();

private:
    struct [[gnu::packed]] Entry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  flags_limit_high;
        uint8_t  base_high;
    };
    static_assert(sizeof(Entry) == 8, "GDT entry must be 8 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    struct [[gnu::packed]] TaskStateSegment {
        uint32_t reserved0;
        uint64_t rsp[3];
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    };
    static_assert(sizeof(TaskStateSegment) == 104, "TSS must be 104 bytes");
```

`Entry` 结构体严格按照 Intel SDM 的字节布局——8 字节，6 个字段，没有任何抽象层。`[[gnu::packed]]` 告诉编译器不要在字段之间插入 padding（比如在 `access` 和 `flags_limit_high` 之间），`static_assert(sizeof(Entry) == 8)` 在编译期验证。这两个防护措施非常重要——如果哪天你把某个字段从 `uint8_t` 改成了 `uint16_t`，`static_assert` 会立刻报错，而不是等到运行时 triple fault 才发现。说实话这个坑真的很难排查，因为编译器不会帮你检查结构体和硬件期望的对应关系。

`TaskStateSegment` 结构体严格按照 Intel SDM Vol.3A §7.7 Table 7-8 的 64 位 TSS 布局定义，总共 104 字节。字段包括 reserved0（4 字节）、RSP 数组（3 个 64 位值，对应 Ring 0/1/2 的栈指针）、reserved1（8 字节）、IST 数组（7 个 64 位值）、reserved2 + reserved3 + iomap_base。

然后是工厂函数和成员变量：

```cpp
    static constexpr Entry null_entry() { return {0, 0, 0, 0, 0, 0}; }

    static constexpr Entry segment_entry(SegmentAccess access, SegmentFlags flags) {
        return {
            .limit_low        = 0xFFFF,
            .base_low         = 0,
            .base_middle      = 0,
            .access           = static_cast<uint8_t>(access),
            .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
            .base_high        = 0,
        };
    }

    static constexpr Entry tss_low_entry(uint64_t base, uint32_t limit);
    static constexpr Entry tss_high_entry(uint64_t base);

    static constexpr auto kEntryCount = 7;
    Entry entries_[kEntryCount]{};
    Pointer gdtr_{};
    TaskStateSegment tss_{};

    void load();
};

extern GDT g_gdt;
```

`segment_entry()` 的 `flags_limit_high` 字段构造方式是 `(flags << 4) | 0x0F`——高 4 位放 flags（左移 4 位），低 4 位放 limit 的高 4 位（固定 0x0F）。配合 `limit_low = 0xFFFF`，总 limit 为 `0xFFFFF`，再配合 `Granularity4K` 粒度，覆盖完整的 4GB 地址空间。Base 全部填 0——在 long mode 下硬件忽略 base 和 limit，但填 0 是最安全的做法。

`entries_`、`gdtr_`、`tss_` 都用 `{}` 初始化（zero-initialized），确保 `init()` 被调用之前所有字段都是 0。

## 第二步——填充描述符、lgdt、刷新段寄存器

现在我们来实现 `gdt.cpp`。`GDT::init()` 填充 7 个条目，然后调用 `load()` 把设置加载到硬件。

```cpp
namespace cinux::arch {

GDT g_gdt;

void GDT::init() {
    entries_[0] = null_entry();

    entries_[1] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[6] = tss_high_entry(tss_addr);
```

你会发现这段代码的可读性比直接写十六进制数值好太多了——`SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite` 一眼就能看出"这是一个 Present 的、Ring 0 的、代码段的、可执行的、可读的描述符"，而 `0x9A` 就没那么直观了。对比 mini kernel 时代我们用 `0x00CF9A000000FFFF` 定义描述符的做法，现在的方案虽然写起来更长，但维护起来舒服得多。

对比 entries_[1]（kernel code64）和 entries_[2]（kernel data64）的 flags：code64 用 `LongMode`（L=1, D=0），data64 用 `Size32`（L=0, D/B=1）。对比 entries_[1] 和 entries_[3]（user code64）：唯一的区别是 `Ring0` vs `Ring3`。

TSS 描述符占两个 slot——`tss_low_entry` 生成前 8 字节（包含 limit、base 低 32 位、access byte `Present|TSS64Avail`=0x89），`tss_high_entry` 生成后 8 字节（包含 base 高 32 位）。64 位 TSS 描述符之所以需要 16 字节，是因为 TSS 的基地址是 64 位的——需要额外的 8 字节存放高 32 位地址。这是 x86_64 特有的设计，32 位模式下 TSS 描述符只有 8 字节。

```cpp
    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

GDTR 的 limit 是 55（7 * 8 - 1），base 指向 `entries_` 数组的首地址。然后调用 `load()` 执行硬件操作。

```cpp
void GDT::load() {
    __asm__ volatile(
        "lgdt %[gdtr]\n\t"
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(gdtr_), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    const uint16_t tss_sel = GDT_TSS;
    __asm__ volatile("ltr %[sel]\n\t" : : [sel] "r"(tss_sel) : "memory");
}
```

这段内联汇编做了三件事，我们来逐行拆解。`lgdt %[gdtr]` 从内存加载 GDTR——limit 和 base 都从 `gdtr_` 结构体中读取。但 `lgdt` 只更新了 GDTR 寄存器的值，CS 的"隐藏部分"（段描述符缓存）仍然是旧的——CPU 不会主动去新 GDT 中重新读取 CS 对应的描述符。所以接下来我们用"伪造的 far return"来强制刷新 CS：先把新的 CS 值（0x08）压栈，再把返回地址（`1f` 标签的位置，通过 `leaq 1f(%%rip)` 计算）压栈，然后 `lretq`。`lretq` 从栈上弹出 RIP 和 CS，完成 CS 的重新加载。这一步不能用 `mov` 指令替代——CS 只能通过 far jump、far call、far return 或 IRET 来加载。

DS/ES/FS/GS/SS 的刷新就简单多了——直接 `mov` 加载即可。先加载到 AX（因为不能把立即数直接 mov 到段寄存器），然后从 AX 复制到各段寄存器。最后 `ltr` 加载任务寄存器，把 TSS 选择子（0x28）加载到 TR。`ltr` 会自动把 GDT 中对应位置的 TSS 描述符标记为 "Busy"。

这里有一个细节值得注意：内联汇编的 clobber 列表里有 `"rax"` 和 `"memory"`。`"rax"` 是因为我们用 RAX 做了中转（`leaq ... %%rax` 和 `movw ... %%ax`），编译器不能假设 RAX 的值在汇编前后不变。`"memory"` 更重要——`lgdt` 改变了内存段的描述方式，`lretq` 改变了代码段属性，编译器必须假设所有缓存的内存访问都失效了，不能把汇编前的内存读取消点到汇编之后。

## 验证

编译并运行大内核测试，段寄存器的值应该和预期一致：

```bash
cmake --build build --target run-big-kernel-test
```

预期输出中 `[PASS] test_cs_register`、`[PASS] test_ds_register` 等测试全部通过，说明 CS=0x08、DS=0x10、SS=0x10、ES=0x10，GDT 加载和段寄存器刷新全部正确。

## 收尾

到这里，大内核有了一个完整的 GDT，七个描述符全部到位，段寄存器和任务寄存器也都正确加载了。这是 IDT 和中断处理的基础——没有正确的 GDT，IDT 中的代码段选择子就无处可指，中断处理根本无法工作。

下一节我们要配置 IDT、编写 ISR 汇编跳板、实现异常处理函数，然后用一条 `int $3` 指令触发断点异常，验证整个中断处理链路：CPU 异常 → ISR stub → C handler → 寄存器 dump → 程序继续执行。那是这一章真正"看到效果"的部分。

## 参考资料

- Intel SDM: Vol.3A §3.4.5 — Segment Descriptors (Figure 3-8)
- Intel SDM: Vol.3A §2.4.1 — GDTR/LGDT (Figure 2-6)
- Intel SDM: Vol.3A §7.7 — Task State Segment in 64-bit Mode (Table 7-8)
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- OSDev Wiki: [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial)
- Linux: [arch/x86/include/asm/desc.h](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/desc.h) — per-CPU GDT, set_tssldt_descriptor
- Linux: [arch/x86/include/asm/processor.h](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/processor.h) — x86_hw_tss struct (104 bytes)
- xv6: [bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — flat GDT, SEG_ASM macro
- xv6: [mmu.h](https://github.com/mit-pdos/xv6-public/blob/master/mmu.h) — segment descriptor macros
