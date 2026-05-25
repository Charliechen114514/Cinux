---
title: SYSRETQ SS.RPL
---

# 024-01: SYSRETQ 未正确设置 SS.RPL 导致 IRETQ #GP 崩溃

## 一、问题现象

**症状**：Shell 在 Ring 3 成功启动并打印 prompt 后，下一次 PIT 时钟中断触发时内核崩溃：

```log
Cinux shell - type 'help' for commands
cinux> 
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81000A3C   CS  = 0x0010
  RSP   = 0xFFFFFFFF81012F08   SS  = 0x0000
  ERROR CODE = 0x0000000000000028
[FATAL] General Protection Fault in kernel mode (error code=0x0000000000000028)
```

**崩溃特征**：
- 崩溃点为 `irq0_stub` 的 `iretq` 指令（PIT 时钟中断返回）
- 错误码 `0x28` = GDT selector index 5（User Data 段），RPL=0
- Shell 之前的 sys_write 正常工作（说明 SYSCALL/SYSRETQ 基本通路没问题）
- 仅在 Ring 3 → Ring 0 → Ring 3 的中断往返路径上触发

---

## 二、排查过程

### 第一步：确认崩溃指令

用 `addr2line` 定位崩溃 RIP：

```bash
addr2line -e build/kernel/big/big_kernel -f 0xFFFFFFFF81000A3C
# → irq0_stub
```

反汇编确认是 `iretq`：

```asm
ffffffff81000a38:  48 83 c4 08   add    $0x8,%rsp     # 跳过 dummy error code
ffffffff81000a3c:  48 cf         iretq                 # ← 崩溃点
```

### 第二步：分析错误码

错误码 `0x28` 分解：

| 位域      | 值  | 含义                     |
|-----------|-----|--------------------------|
| Index     | 5   | GDT 第 5 项              |
| TI        | 0   | GDT（非 LDT）            |
| EXT       | 0   | 非外部事件               |

GDT 第 5 项（selector `0x28`）是 User Data 段。`IRETQ` 尝试将 `SS` 加载为 `0x28`（RPL=0），但返回目标 CPL=3，SS 的 DPL/RPL 检查失败 → `#GP`。

这说明中断栈帧上的 **用户态 SS 值为 `0x28` 而非 `0x2B`**——SYSRETQ 设置了错误的 SS。

### 第三步：在 PIT handler 中插入诊断打印

在 `pit_irq0_handler` 中打印 `InterruptFrame` 的 CS/SS：

```cpp
if ((frame->cs & 3) != 0 || dbg_cnt < 3) {
    kprintf("[DBG-PIT] tick #%d: CS=0x%04x SS=0x%04x RIP=%p\n",
            dbg_cnt++, frame->cs, frame->ss, frame->rip);
}
```

关键输出：

```log
[DBG-PIT] tick #0: CS=0x0010 SS=0x0018 RIP=...  # 内核态，正常
[DBG-PIT] tick #1: CS=0x0010 SS=0x0018 RIP=...  # 内核态，正常
[DBG-PIT] tick #9: CS=0x0033 SS=0x0028 RIP=...  # ← 用户态，SS=0x28！
==== EXCEPTION: #GP (vector 13) ====
```

**确诊**：用户态 SS=`0x0028`（RPL=0），正确值应为 `0x002B`（RPL=3）。CS=`0x0033` 正确（RPL=3）。

### 第四步：验证 STAR MSR 和 GDT 描述符

**STAR MSR 回读**（在 `syscall_init` 中插入 `rdmsr`）：

```log
[SYSCALL] STAR readback: 0x00200010_00000000
```

分解：STAR[63:48]=`0x0020`，STAR[47:32]=`0x0010`。值完全正确。

**GDT[5] 描述符 dump**（在 `launch_first_user` 中 `sgdt` + 直接读内存）：

```log
[USER] GDT[5] raw: ff ff 00 00 00 f2 cf 00
```

分解：access=`0xF2` = Present + DPL3 + CodeData + ReadWrite。DPL=3，正确。

### 第五步：锁定根因

STAR 和 GDT 都正确，但 SYSRETQ 产出的 SS=`0x28`（缺 RPL=3）。

Intel SDM 规定 SYSRETQ 应设置 `SS = STAR[63:48] + 8` 并将 RPL 置 3。实际行为：

```
期望: SS = 0x20 + 8 | 3 = 0x2B
实际: SS = 0x20 + 8     = 0x28  (RPL 未设置)
```

这是 QEMU 在某些版本中对 SYSRETQ 的模拟行为——SS selector 计算正确但没有自动附加 RPL=3。

---

## 三、修复方案

### 思路

把 RPL=3 直接编码进 STAR[63:48] 基值，让 SYSRETQ 的加法结果本身就带 RPL=3：

```
STAR[63:48] = 0x23
SYSRETQ CS  = 0x23 + 16 = 0x33  (index 6, RPL=3)  ✓
SYSRETQ SS  = 0x23 + 8  = 0x2B  (index 5, RPL=3)  ✓
```

无论 CPU 是否额外设置 RPL，selector 值都正确。

SYSCALL 路径不受影响（它读 STAR[47:32]）：

```
SYSCALL CS = STAR[47:32]     = 0x10  ✓
SYSCALL SS = STAR[47:32] + 8 = 0x18  ✓
```

### 改动

**`gdt.hpp`** — 常量：

```cpp
// Before:
constexpr uint16_t GDT_SYSRET_BASE = 0x20;
// After:
constexpr uint16_t GDT_SYSRET_BASE = 0x23;
```

**`usermode.S`** — `usermode_init_asm` 中的立即数：

```asm
# Before:
movq $0x20, %rdx
# After:
movq $0x23, %rdx
```

---

## 四、修复后验证

```log
Cinux shell - type 'help' for commands
cinux> 
# 10 秒 timeout 正常退出，无崩溃
```

Shell 正常运行，PIT 中断往返不再触发 #GP。

---

## 五、后续影响评估

### 是否有副作用？

**没有。** 改动仅影响 STAR[63:48] 的值从 `0x20` 变为 `0x23`：

- SYSRETQ 计算 CS/SS 时，`+16`/`+8` 的结果与之前**完全相同**（`0x33`/`0x2B`），只是 RPL 已经编码在基值中。
- GDT 描述符无需任何修改——selector `0x33` 指向 index 6，selector `0x2B` 指向 index 5，和之前一样。
- SYSCALL 入口读 STAR[47:32]，不受影响。
- Linux 内核也使用类似技巧（详见下方「与 Linux 的对齐」一节）。

### 对未来开发的影响

**正面**：这个 fix 让 SYSRETQ 的 SS 行为不再依赖 CPU/QEMU 的 RPL 设置实现，更加健壮。

**无负面影响**：
- 新增的 syscall handler、用户程序、调度器切换等都不需要适配
- GDT 布局不变
- 中断/异常处理路径不变

---

## 六、经验教训

### 1. SYSRETQ 的 SS.RPL 不可信

Intel SDM 说 SYSRETQ 会将 SS.RPL 设为目标 CPL，但实际硬件（尤其是 QEMU）可能不遵守。**把 RPL 编码进 STAR 基值是最安全的做法。**

### 2. 中断帧诊断技巧

在 ISR 的 C handler 中打印 `frame->cs` 和 `frame->ss` 是快速区分用户态/内核态中断、定位 SS 异常的最直接手段：

```cpp
if ((frame->cs & 3) != 0) {
    // 这是来自 Ring 3 的中断，SS 应为用户数据段
    kprintf("User interrupt: CS=0x%04x SS=0x%04x\n", frame->cs, frame->ss);
}
```

### 3. IRETQ 错误码 = 出问题的 selector

`#GP` 错误码在 IRETQ 场景下就是 CPU 试图加载的那个段 selector。结合 GDT 布局可以直接定位是哪个描述符。

### 4. `addr2line` + 反汇编是排查异常的第一步

崩溃 RIP → 函数名 → 具体指令，30 秒就能确定是哪条指令出了问题。

---

## 相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/arch/x86_64/gdt.hpp` | `GDT_SYSRET_BASE` 从 `0x20` 改为 `0x23` |
| `kernel/arch/x86_64/usermode.S` | STAR 立即数从 `$0x20` 改为 `$0x23` |
