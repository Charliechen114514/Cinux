---
title: 022-ring3-usermode-1 · 用户态 (Ring 3)
---

# 022-1 Read-through · TSS/IST/IDT 基础设施改造

## 概览

本文覆盖 tag 022 中 TSS、IST 和 IDT 的基础设施改造。这些改动为 Ring 3 切换提供了必要的硬件支持：TSS 的 IST1 给 Double Fault 提供独立栈，IDT 路由表增加了 IST 字段，#GP 处理函数增加了用户态/内核态的区分。虽然这些改动本身不会直接进入 Ring 3，但没有它们，Ring 3 的异常处理路径就是不完整的。

关键设计决策一览：使用静态分配的 4KB 栈作为 IST1 Double Fault 栈（避免运行时分配失败）；IDT 路由表从 4 字段扩展为 5 字段（新增 IST 编号）；#GP 处理通过 CS 低两位判断中断来源。

## 架构图

```
                    IDT Entry (16 bytes)
                 +---+---+---+---+---+
                 | offset[0:15]  |seg|  IST=0 or 1
                 +---+---+---+---+---+
                 | attr      |offset  |
                 +---+---+---+---+---+

    GDT (9 entries)            TSS (104 bytes)
  +---------+              +------------+
  | 0: null |              | reserved0  | 0x00
  | 1: TLS  |              | RSP0       | 0x04  <--- tss_set_rsp0()
  | 2: cs0  | 0x10         | RSP1       | 0x0C
  | 3: ds0  | 0x18         | RSP2       | 0x14
  | 4: cs32 | 0x20         | reserved   | 0x1C
  | 5: ds3  | 0x28         | IST1       | 0x24  <--- df_stack_ top
  | 6: cs64 | 0x30         | IST2-IST7  | 0x2C
  | 7: TSS  |----->        | reserved   | 0x5C
  | 8: TSS  |  (16 bytes)  | IOPB       | 0x64
  +---------+              +------------+
```

## 代码精讲

### TSS 结构和 Double Fault 栈 (gdt.hpp)

TSS 结构定义在 GDT 类的 private 区域，使用 `[[gnu::packed]]` 确保没有填充字节：

```cpp
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

这个布局精确对应 Intel SDM Vol.3A Section 8.2.1 定义的 64-bit TSS 结构。`rsp[0]` 就是 RSP0——Ring 3 发生特权级提升时 CPU 自动加载的栈指针。`ist[0]` 就是 IST1——IDT 中指定 IST=1 的中断使用这个栈。

注意 `iomap_base` 字段——当 TSS 描述符的 IOPB 值 >= TSS limit 时，I/O 权限位图被视作不存在，所有 I/O 端口访问由 CPL 和 IOPL 决定。Cinux 当前不使用 IOPB，这个值通常设为 sizeof(TSS) = 104（即指向 TSS 末尾，表示空位图）。

接下来是 Double Fault 栈的声明，同样是 GDT 类的成员：

```cpp
static constexpr uint64_t DF_STACK_PAGES = 1;
uint64_t df_stack_phys_{};
alignas(16) uint8_t df_stack_[DF_STACK_PAGES * 4096]{};
```

4KB 的静态分配栈，16 字节对齐（x86-64 ABI 对栈对齐的要求）。这个栈编译时就存在，不存在运行时分配失败的可能性——对于 Double Fault 这种最严重的异常场景，可靠性比灵活性重要。

### IST1 初始化 (gdt.cpp)

在 GDT::init() 中，IST1 被设置为 Double Fault 栈的顶部：

```cpp
tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);
```

栈从高地址向低地址增长，所以 IST1 指向缓冲区的末尾（最后一个字节之后的地址）。当 #DF 触发时，CPU 从 IST1 加载 RSP，第一次 push 就会写入 `df_stack_[4096-8]` 的位置。

这一行必须在 TSS 描述符写入 GDT 之前执行——因为 GDT 描述符的 base 地址就是 &tss_ 的地址，一旦 ltr 执行，CPU 就可能通过 TSS 访问 IST 字段。

### IDT IST 字段 (idt.cpp)

IDT 路由结构从 4 字段扩展为 5 字段：

```cpp
struct Route {
    ExceptionVector vector;
    Stub stub;
    IDTPrivilege priv;
    IDTGateType gate;
    uint8_t ist;    // <-- 新增
};
```

路由表中，只有 #DF (Double Fault) 使用 IST=1：

```cpp
{ExceptionVector::DF, isr_df_stub, IDTPrivilege::Kernel,
 IDTGateType::Interrupt, 1},
```

其余所有异常的 IST 都是 0（使用当前 RSP）。这个分配和 Linux 的做法一致——Linux 还给 #NMI 分配了 IST2，给 #MC (Machine Check) 分配了 IST3，但 Cinux 目前只有 #DF 使用 IST。

`set_handler` 的调用也相应修改，把 `ist` 值传入：

```cpp
set_handler(r.vector, r.stub, GDT_KERNEL_CODE,
            make_idt_attr(r.priv, r.gate), r.ist);
```

### #GP 用户态检测 (exception_handlers.cpp)

handle_gp 函数从简单的 "fatal halt" 改为区分中断来源：

```cpp
void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);
    bool from_user = (frame->cs & 0x03) != 0;

    if (from_user) {
        kprintf("[EXCEPTION] #GP at RIP=%p from user mode (Ring 3)\n",
                reinterpret_cast<void*>(frame->rip));
        kprintf("[EXCEPTION] Privileged instruction executed in Ring 3"
                " -- protection works!\n");
    } else {
        kprintf("[FATAL] General Protection Fault in kernel mode"
                " (error code=%p)\n",
                reinterpret_cast<void*>(frame->error_code));
    }
    fatal_halt();
}
```

这里的判断逻辑是检查 CS 寄存器低两位（CPL 字段）。Ring 0 的 CS=0x10（低两位=0），Ring 3 的 CS=0x33（低两位=3）。当用户程序执行 `cli` 触发 #GP 时，InterruptFrame 中的 CS 保存的是触发异常时的 CS 值（0x33），所以 `frame->cs & 0x03 == 3`，`from_user` 为 true。这个检测方式在 x86-64 上是标准做法——Linux 内核的 `user_mode(regs)` 宏做的是完全相同的事情。

## 设计决策

### 决策：Double Fault 栈的分配方式

**问题**：IST1 栈如何分配——静态分配还是动态分配？

**本项目的做法**：作为 GDT 类的静态成员数组（4KB），编译时分配。

**备选方案**：从 PMM 动态分配一个物理页。Linux 就是为每个 CPU 动态分配 Double Fault 栈。

**为什么不选备选方案**：Cinux 当前是单核，GDT 只有一个实例。动态分配增加了失败的可能性，而 Double Fault 的处理路径必须 100% 可靠。静态分配虽然浪费了 4KB 的 BSS 空间，但消除了分配失败的风险。对于教学内核来说，这个取舍很合理。

**如果要扩展/改进**：多核支持时，每个 CPU 需要自己的 GDT/TSS 实例和 Double Fault 栈。可以改为 per-CPU 结构体中动态分配，或者在启动时为每个 CPU 预分配。Linux 的做法是为每个 CPU 分配 `cpu_entry_area`，其中包含独立的 Double Fault 栈和 IST 栈。

### 决策：#GP handler 的恢复策略

**问题**：Ring 3 触发 #GP 后应该恢复还是终止？

**本项目的做法**：两种情况都调用 `fatal_halt()`——无论 #GP 来自内核还是用户，系统都停机。

**备选方案**：来自 Ring 3 的 #GP 应该终止触发进程（发送 SIGSEGV），而不是终止整个系统。

**为什么不选备选方案**：Cinux 当前没有进程退出机制（进程创建在 tag 019，但进程终止尚未实现）。等到实现进程退出后，可以从 `from_user` 分支调用 `process_exit(-1)` 代替 `fatal_halt()`。内核态 #GP 则仍然 halt——内核态的 #GP 是严重的编程错误，不应该尝试恢复。

## 扩展方向

- 为 #NMI (vector 2) 和 #MC (vector 18) 也分配 IST 栈（参考 Linux 的 IST2/IST3）
- 实现 #DF 的恢复策略（当前只是 halt，可以尝试重置中断控制器后继续）
- 在 TSS 中设置 IOPB (I/O Permission Bitmap) 来精细控制 Ring 3 的 I/O 端口访问权限
- 支持 per-CPU GDT/TSS，为 SMP 做准备

## 关键偏移量速查

| 字段 | 偏移量 | 大小 | 说明 |
|------|--------|------|------|
| reserved0 | 0x00 | 4 字节 | 保留，必须为 0 |
| RSP0 | 0x04 | 8 字节 | Ring 0 栈指针（CPL 提升时加载） |
| RSP1 | 0x0C | 8 字节 | Ring 1 栈指针（Cinux 不使用） |
| RSP2 | 0x14 | 8 字节 | Ring 2 栈指针（Cinux 不使用） |
| reserved1 | 0x1C | 8 字节 | 保留 |
| IST1 | 0x24 | 8 字节 | 中断栈表 1（Double Fault） |
| IST2 | 0x2C | 8 字节 | 中断栈表 2（Cinux 不使用） |
| IST3-7 | 0x34+ | 5*8 字节 | 中断栈表 3-7（Cinux 不使用） |
| reserved2 | 0x5C | 8 字节 | 保留 |
| reserved3 | 0x64 | 2 字节 | 保留 |
| IOPB | 0x66 | 2 字节 | I/O 权限位图偏移 |

## 测试覆盖

本章代码有以下测试保障：

**内核测试 (test/test_usermode.cpp)**：
- Test 1-3: TSS RSP0 读写——验证 tss_set_rsp0 基本功能和多次写入
- Test 13-16: 段选择子验证——CS=0x10, DS=SS=0x18, TR=0x38
- Test 24: DF 栈验证——确认 TR 已加载

**Host 单元测试 (test/unit/test_usermode.cpp)**：
- TSS 大小 104 字节、rsp[3] 数组、ist[7] 数组
- 各字段偏移量验证（reserved0=0, rsp=4, ist=36, iomap_base=102）
- TSS 描述符 type=0x89 验证
- GDT_TSS 选择子 index=7 验证
- RSP0/RSP1/RSP2 独立性验证

## 参考资料

- Intel SDM: Vol.3A Section 8.2.1 — Task-State Segment (TSS) in 64-bit mode
- Intel SDM: Vol.3A Section 8.2.3 — TSS Descriptor in 64-bit mode
- Intel SDM: Vol.3A Section 6.14.5 — Interrupt Stack Table (IST) mechanism
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- Linux: arch/x86/entry/entry_64.S — IST assignment for #DF/#NMI/#MC
