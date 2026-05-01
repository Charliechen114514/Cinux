# 010-1 Hands-on: 大内核 GDT 初始化——段描述符工厂与 lgdt

## 导语

上一章（009）我们让大内核跑起来了——串口能输出、kprintf 能格式化打印、压力测试也通过了。但说实话，现在的大内核是个"聋哑人"：CPU 遇到除零、缺页、非法指令这些异常时，只会 triple fault 然后 QEMU 重启，连一行遗言都留不下来。开发内核最痛苦的事情莫过于此：你写了半天代码，跑起来直接黑屏重启，连出问题的地方都不知道。

这一章的前半部分，我们要给大内核建一个全新的 GDT（全局描述符表）。你可能会想——002 章在 Bootloader 里不是配过一次吗？007 章的 mini kernel 不是也配过吗？为什么大内核还要再来一次？原因在于大内核运行在 higher-half 虚拟地址空间，段寄存器和 IDT 中引用的段选择子必须指向大内核自己管理的内存里的 GDT 条目。而且在 x86_64 的 long mode 下，虽然硬件基本忽略了分段机制的基地址和限长，但 CS/DS/SS 这些段寄存器仍然需要指向有效的 GDT 条目，CPU 才能正常工作——尤其是中断处理环节，IDT 里的每个条目都有一个"代码段选择子"字段指向 GDT 中的 code segment 描述符，如果 GDT 没配好，中断一触发就 triple fault。

完成本节后，大内核会拥有一个包含 null/kernel code64/kernel data64/user code64/user data64/TSS 占位的完整 GDT，所有段寄存器都会指向正确的描述符。这个 GDT 是下一节 IDT 和中断处理的前置条件——没有正确的 GDT，IDT 里的代码段选择子就无处可指。

前置知识：上一章（009）的大内核启动流程和 kprintf 串口输出。

---

## 概念精讲

### 为什么 long mode 下还需要 GDT？

在 x86_64 的 long mode 下，分段机制被大幅弱化了——Base 被硬件当作 0，Limit 被当作 `0xFFFFFFFFF`，基本上就是"平坦模式"的硬件实现。那为什么我们还需要配 GDT？原因有两个。

第一，CS/DS/SS 这些段寄存器内部缓存着"隐藏部分"（段描述符缓存），CPU 在做特权级检查、权限验证、长模式代码执行判断时，仍然会读取这些缓存。比如 CPU 看到一个 64 位代码段描述符（L=1, D=0），它才知道当前是 64 位模式；如果 CS 指向的描述符 L=0, D=1，CPU 就按 32 位兼容模式执行代码——然后你的 `iretq` 指令直接变成非法操作。第二，IDT 里的每个条目都有一个 Segment Selector 字段，CPU 在中断处理时会用这个选择子加载 CS。如果 GDT 里没有正确的 code64 描述符，中断触发后 CPU 找不到合法的代码段，直接 #GP → Double Fault → Triple Fault → QEMU 重启。

### 这次 GDT 比 mini kernel 的"豪华"在哪里？

mini kernel 的 GDT 只有三项：null + code32 + data32，纯 32 位保护模式方案。大内核的 GDT 有七项——null、kernel code64、kernel data64、user code64、user data64、TSS low、TSS high。多了三样东西：用户态的 code 和 data 段（为将来跑用户进程做准备，DPL=3），以及一个 TSS（Task State Segment）占位符。

TSS 在 long mode 下的主要用途不是硬件任务切换——那是 32 位时代的遗产，64 位模式下已经不支持了。TSS 在 64 位模式下提供两个功能：一是 IST（Interrupt Stack Table），可以为 NMI、Double Fault 等关键中断提供已知良好的栈；二是 RSP0/RSP1/RSP2，当 CPU 从 Ring 3 切换到 Ring 0 处理中断时，它会自动从 TSS 里读取新的栈指针。我们的 TSS 目前是一个全零的占位符——所有字段都是 0，在设置好 IST 或用户态切换之前它不会起实际作用，但 GDT 里必须有这个条目，否则 `ltr` 指令会触发 #GP。

### GDT 布局与选择子计算

```
大内核的 GDT 布局：

索引  选择子   内容               用途
────  ──────   ────               ────
0     0x00     Null Descriptor    Intel 规定第一项必须为空
1     0x08     Kernel Code64      内核代码段 (Ring 0, 64-bit, L=1)
2     0x10     Kernel Data64      内核数据段 (Ring 0, D/B=1)
3     0x1B     User Code64        用户代码段 (Ring 3, 64-bit, RPL=3)
4     0x23     User Data64        用户数据段 (Ring 3, RPL=3)
5-6   0x28     TSS (16 bytes)     任务状态段占位 (占两个 GDT slot)
```

选择子的值是 `索引 * 8 + RPL`。对于内核态段，RPL=0，所以 Kernel Code64 = 1*8 = 0x08。对于用户态段，RPL=3，所以 User Code64 = 3*8 + 3 = 0x1B，User Data64 = 4*8 + 3 = 0x23。TSS 虽然占了两个 slot（索引 5 和 6），但选择子指向的是索引 5，即 5*8 = 0x28。

### Scoped Enum 与类型安全的描述符构建

段描述符的 access byte 是一个 8 位的位域，不同 bit 代表不同含义。如果直接用 `uint8_t` 传入 access 值，调用者完全可以传入一个语义上毫无意义的值（比如 `0xFF`），编译器不会报错，运行时 triple fault。我们的方案是用 scoped enum（`enum class SegmentAccess : uint8_t`）配合 `operator|` 重载，调用者只能用预定义的标志来组合——`SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData | SegmentAccess::Executable`，这样写即使漏掉了一个标志，编译器的类型系统也能帮你检查出来（至少比传错一个 `uint8_t` 数字好发现得多）。

`SegmentFlags` 对应描述符第 6 字节的高 4 位。这里有个容易混淆的地方——内核数据段的 flags 用的是 `Size32`（D/B=1）而不是 `LongMode`（L=1）。原因是 64 位模式下只有代码段才需要设置 L 位，数据段的 L 位被保留（必须为 0），数据段只需要设置 D/B 位为 1 即可。如果你把数据段的 L 位也置 1，CPU 行为是未定义的——实测中有的 CPU 会 #GP，有的会静默忽略。

---

## 动手实现

### Step 1: 创建 GDT 头文件——类型、枚举与类声明

**目标**: 创建 `kernel/arch/x86_64/gdt.hpp`，定义段选择子常量、`SegmentAccess` 和 `SegmentFlags` scoped enum、`GDT` class（含 Entry/Pointer/TaskStateSegment 内部结构体、constexpr 工厂函数、init/load 方法）。

**设计思路**: 整个 GDT 的状态——描述符数组、GDTR 指针、TSS 结构体——全部封装在一个 `GDT` class 里，对外只暴露 `init()` 方法。这比用全局函数 + 全局数组的做法（比如 mini kernel 的方式）封装性好，将来如果要做 per-CPU GDT（像 Linux 那样），只需要把 `GDT g_gdt` 变成 per-CPU 变量即可。

**实现约束**:
- `Entry` 结构体必须 8 字节，用 `[[gnu::packed]]` 消除 padding，配合 `static_assert(sizeof(Entry) == 8)` 编译期检查
- `TaskStateSegment` 结构体必须 104 字节（Intel SDM Vol.3A Table 8-2 规定），同样用 `static_assert(sizeof(TaskStateSegment) == 104)` 保护
- `Pointer` 结构体 10 字节（2 字节 limit + 8 字节 base），用于 `lgdt` 指令
- 段选择子常量定义为 `constexpr uint16_t`：`GDT_KERNEL_CODE = 0x08`、`GDT_KERNEL_DATA = 0x10`、`GDT_USER_CODE = 0x1B`、`GDT_USER_DATA = 0x23`、`GDT_TSS = 0x28`
- `SegmentAccess` 枚举需要包含：`Present`(bit7)、`Ring0`(0)、`Ring3`(3<<5)、`CodeData`(bit4)、`Executable`(bit3)、`ReadWrite`(bit1)、`TSS64Avail`(0x09)
- `SegmentFlags` 枚举需要包含：`Granularity4K`(bit3)、`LongMode`(bit1)、`Size32`(bit2)
- 两个枚举都需要 `operator|` 重载以支持组合
- 工厂函数：`null_entry()` 返回全零描述符、`segment_entry(access, flags)` 生成代码/数据段描述符、`tss_low_entry(base, limit)` 和 `tss_high_entry(base)` 生成 TSS 的两个 8 字节

**踩坑预警**:
- 如果忘了 `[[gnu::packed]]`，编译器可能在 Entry 的 `access`(uint8_t) 和 `flags_limit_high`(uint8_t) 之间插入 padding，导致结构体超过 8 字节。`static_assert` 会帮你抓住这个错误。
- `SegmentFlags::Size32` 的值是 `1u << 2`（即 bit 2，对应 D/B 位），不是 bit 1。别和 LongMode 的 bit 1 搞混。
- TSS 描述符的 access byte 使用的是完整的值 `0x89`（Present=1, 64-bit TSS Available），不是按 bit 组合的——所以 `TSS64Avail = 0x09` 是一个预设值。

**验证**: 此步完成后编译应该通过。可以用 `static_assert` 验证各工厂函数生成的描述符值是否符合预期（比如 kernel code64 的 access 应该等于 0x9A）。

```bash
# 编译验证
cmake --build build --target big_kernel 2>&1 | tail -5
```

### Step 2: 实现 GDT 初始化——填充描述符、lgdt、刷新段寄存器、ltr

**目标**: 实现 `kernel/arch/x86_64/gdt.cpp`，包含全局 `g_gdt` 实例、`GDT::init()` 和 `GDT::load()` 方法。

**设计思路**: `init()` 填充 7 个 GDT 条目后调用 `load()`。`load()` 是最关键的部分——它不仅执行 `lgdt` 加载 GDTR，还必须刷新所有段寄存器。CS 的刷新最为特殊：不能简单地 `mov` 到 CS，必须通过"伪造一个 far return"来实现——先把新的 CS 值压栈，再把返回地址压栈，然后 `lretq`。这样 CPU 在执行 lretq 时会从栈上弹出新 CS 和 RIP，完成 CS 的重新加载。DS/ES/FS/GS/SS 则可以直接 `mov` 加载。最后执行 `ltr` 加载任务寄存器。

**实现约束**:
- `GDT::init()` 填充顺序：entries_[0] = null, entries_[1] = kernel code64, entries_[2] = kernel data64, entries_[3] = user code64, entries_[4] = user data64, entries_[5] = TSS low, entries_[6] = TSS high
- Kernel code64: `Present | Ring0 | CodeData | Executable | ReadWrite`, flags = `Granularity4K | LongMode`
- Kernel data64: `Present | Ring0 | CodeData | ReadWrite`, flags = `Granularity4K | Size32`（注意不是 LongMode）
- User code64: 和 kernel code64 类似，但把 `Ring0` 换成 `Ring3`
- User data64: 和 kernel data64 类似，但把 `Ring0` 换成 `Ring3`
- TSS: `tss_low_entry` 的 access 自动设为 `Present | TSS64Avail`，limit 为 `sizeof(TaskStateSegment) - 1`
- GDTR 的 limit = `sizeof(entries_) - 1`（7 个 8 字节 entry = 56 字节，limit = 55），base 指向 entries_ 数组的地址
- 内联汇编中的 `lgdt` 操作数用 `"m"` 约束（内存操作数），CS 和 DS 用 `"i"` 约束（立即数）
- `ltr` 接受的是一个 16 位选择子值（0x28），用 `"r"` 约束传入寄存器

**踩坑预警**:
- `lgdt` 后如果不做 far return 刷新 CS，CS 的隐藏缓存仍然是旧的描述符属性。CPU 内部已经切换了 GDTR 指向新 GDT，但 CS 缓存还没更新——此时任何涉及 CS 的操作（包括中断处理）都可能出问题。
- `lretq` 前的压栈顺序必须是：先压 CS（`pushq %[cs]`），再压 RIP（`leaq 1f(%%rip), %%rax; pushq %%rax`），这样 lretq 弹出时先弹 RIP 再弹 CS，顺序正确。
- `ltr` 会把 TSS 描述符标记为 "busy"，如果 GDT 里的 TSS 条目不存在或格式不对，`ltr` 直接触发 #GP。
- 在内联汇编中，`"memory"` clobber 必须加——因为 lgdt/lretq/mov-to-segment-reg 都会影响内存访问方式，编译器不能假设内存布局不变。

**验证**: 完成此步后，可以在 `kernel_main` 中调用 `g_gdt.init()`，然后读回段寄存器验证值是否正确。

```bash
# 编译并运行
cmake --build build --target big-kernel-test-image
cmake --build build --target run-big-kernel-test
# 预期：串口输出显示 CS=0x08, DS=0x10, SS=0x10
```

---

## 构建与运行

完整的构建和测试命令：

```bash
# 编译大内核测试二进制
cmake --build build --target big-kernel-test-image

# 运行测试（QEMU 自动退出）
cmake --build build --target run-big-kernel-test

# 或者运行生产大内核（手动 Ctrl+C 退出）
cmake --build build --target run
```

QEMU 启动参数说明：测试目标使用 `isa-debug-exit` 设备（port 0xf4），测试通过写 0 退出（QEMU exit code 1），失败写 1 退出（QEMU exit code 3）。

---

## 调试技巧

**问题: lgdt 后 triple fault**
最常见的原因是 GDTR 的 base 地址指向了错误的位置。用 QEMU monitor 的 `info registers` 命令查看 GDTR 的值，确认 base 指向你的 GDT 数组。另一个常见原因是 GDTR 的 limit 算错了——应该是 `sizeof(entries) - 1`，不是 `sizeof(entries)`。

**问题: ltr 触发 #GP**
检查 GDT 中 TSS 描述符的格式。64 位 TSS 描述符占 16 字节（两个 slot），第二个 slot 的高 32 位是 TSS 基地址的高 32 位。如果 TSS 地址本身不对（比如指向了未映射的内存），也会出问题。

**问题: 段寄存器值不是预期的**
检查 `lretq` 前的压栈顺序是否正确（CS 先压、RIP 后压）。用 QEMU monitor 的 `info registers` 可以看到所有段寄存器的值。

```bash
# 串口日志查看（如果使用 -serial file:serial.log）
cat build/serial.log

# GDB 调试
cmake --build build --target run-gdb
# 另一个终端：
gdb build/kernel/big/big_kernel
# (gdb) target remote :1234
# (gdb) break GDT::load
# (gdb) info registers  # 查看 GDTR
```

---

## 本章小结

| 概念 | 要点 |
|------|------|
| GDT in Long Mode | Base/Limit 被忽略，但仍需有效描述符供 CS/DS/SS 使用 |
| 段选择子 | `索引 * 8 + RPL`，内核 RPL=0，用户 RPL=3 |
| Kernel Code64 | L=1, D=0, G=1, Access=Present\|Ring0\|Code\|Exec\|Read |
| Kernel Data64 | L=0, D/B=1, G=1, Access=Present\|Ring0\|Data\|Write |
| User Code64/Data64 | 与 kernel 版相同但 DPL=3 (Ring3) |
| TSS (64-bit) | 104 字节，占 2 个 GDT slot，提供 RSP0/IST |
| lgdt | 加载 GDTR，但不刷新 CS |
| lretq | 远返回，用于刷新 CS 的段描述符缓存 |
| ltr | 加载任务寄存器，标记 TSS 为 busy |
| Scoped Enum | 类型安全的描述符标志组合 |
| static_assert | 编译期验证结构体大小（Entry=8, TSS=104） |
