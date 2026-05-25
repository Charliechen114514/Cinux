---
title: SSE 未初始化崩溃
---

# 011-01: SSE 未初始化导致 -O2 构建崩溃 — Triple Fault 排查

## 一、问题现象

**症状**：使用 `CMAKE_BUILD_TYPE=Release`（`-O2`）构建内核测试时，小内核在 IDT 初始化阶段 **Triple Fault** 崩溃。而 `Debug` 构建类型（`-O0`）一切正常。

```log
=== Tests: 4 passed, 0 failed ===
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
QEMU unexpected exit code: 0        ← Triple Fault，-no-reboot 导致直接退出
```

**崩溃特征**：
- "Setting up IDT..." 之后的输出 **全部消失**
- QEMU 退出码 0（非 isa-debug-exit 的奇数退出码，说明是 Triple Fault）
- Debug 模式（`-O0`）完全不受影响
- 之前所有测试（kprintf / C++ 运行时 / GDT）全部通过

---

## 二、定位崩溃点

### 排查手段

1. 在 `idt_init()` 各步骤间插入 `outb` 到 debugcon（端口 `0xE9`）作为标记
2. 通过 `debug.log` 观察执行到了哪一步

### 关键发现

```
debug.log 输出：OPLJ1234...0    ← '0' 之后再也没有其他标记
```

崩溃点精确定位：**`idt_init()` 的 IDT 清零循环**，即第一个使用 SSE 指令的位置。

### 反编译对比

`-O2` 构建中，编译器将结构体清零循环向量化为 SSE 指令：

```asm
; -O2 生成的 idt_init() 清零循环
ffffffff800239e3:   pxor   %xmm0,%xmm0          ; ← 第一条 SSE 指令，就在这里崩溃
ffffffff800239ee:   movaps %xmm0,(%rcx,%rdx,1)   ; 16 字节对齐写入
```

`-O0` 构建中，编译器生成逐字节的普通 store 指令，完全不使用 SSE。

---

## 三、根本原因：CR4.OSFXSR 未设置

### CPU 寄存器状态

通过在崩溃前读取 CR0 和 CR4 并输出到 debugcon：

```
CR0 = 0x80000011    → PG=1, PE=1, ET=1（TS=0, EM=0）
CR4 = 0x00000020    → PAE=1（OSFXSR=0, OSXMMEXCPT=0）
```

### Intel SDM 的规定

> **`PXOR` / `MOVAPS` 等 128-bit SSE 指令**：若 `CR4.OSFXSR[bit 9] = 0`，触发 `#UD`（Invalid Opcode, Vector 6）。
> 此规则在 **64-bit 长模式**下同样适用。

### 崩溃链条

```
boot.S 入口 → cli → (未设置 CR4.OSFXSR) → ... → idt_init()
                                                    ↓
                                              pxor %xmm0, %xmm0
                                                    ↓
                                          CR4.OSFXSR = 0 → #UD (vector 6)
                                                    ↓
                                          IDT 未加载 (limit=0) → Triple Fault
                                                    ↓
                                          QEMU -no-reboot → exit(0)
```

### 为什么 -O0 没问题？

| 构建类型 | 结构体清零生成方式 | 是否使用 SSE | 结果 |
|----------|-------------------|-------------|------|
| `-O0` | 逐字段 `movb`/`movw` store | ❌ 不使用 | 正常 |
| `-O2` | `pxor` + `movaps` 向量化 | ✅ 使用 | 崩溃 |

### Bootloader 的 SSE 初始化缺失

`long_mode.S` 的 `enter_long_mode` 仅设置了 CR4.PAE（位 5）以启用长模式分页，从未设置：

| CR4 位 | 名称 | 作用 | 状态 |
|--------|------|------|------|
| bit 9 | OSFXSR | 允许 OS 使用 FXSAVE/FXRSTOR 管理 SSE 状态 | ❌ 未设置 |
| bit 10 | OSXMMEXCPT | 允许 SIMD 浮点异常传递为 #XF | ❌ 未设置 |

同时 CR0.TS（位 3）虽然此刻为 0，但也不应在内核入口处假定其为 0。

---

## 四、修复方案

在 `boot.S` 的 `_start` 入口、`cli` 之后立即初始化 SSE：

```asm
_start:
    cli

    /* Enable SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: enable SIMD #XF */
    movq %rax, %cr4
    clts                          /* Clear CR0.TS (Task Switched) */
```

### 为什么放在 boot.S 而不是 long_mode.S？

1. **boot.S 是内核真正的入口点** — SSE 初始化应在内核最早可执行处完成
2. **所有后续代码都可能受益于 SSE** — 编译器在 `-O2` 下可能在任何函数中使用 SSE
3. **`clts` 确保对 CR0.TS 的确定性** — 不依赖 BIOS/KVM 对 CR0 的初始设置

---

## 五、修复后测试输出

```log
=== Tests: 22 passed, 0 failed ===

[TEST] ALL TESTS PASSED (exit code 0)
```

所有 22 项测试（kprintf / C++ 运行时 / GDT/IDT / 中断 / PMM / ATA / ELF Loader / Big Kernel Load / PIC/PIT）全部通过。

---

## 六、经验教训

### 1. 64-bit 长模式 ≠ SSE 自动可用

长模式硬件上支持 SSE（是架构强制要求的），但 **CPU 仍然检查 CR4.OSFXSR**。OS 必须在引导早期显式设置此位，否则 128-bit SSE 指令触发 `#UD`。

```
❌ 误区：64-bit 模式下 SSE 指令一定能执行
✅ 事实：CR4.OSFXSR = 0 时，128-bit SSE 操作 → #UD
```

### 2. 调试 -O0 vs -O2 崩溃的关键思路

当遇到仅 `-O2` 才出现的内核崩溃时：

1. **反汇编对比** — 检查编译器是否生成了 `-O0` 不存在的特殊指令（SSE/AVX/向量化的内存操作）
2. **CR0/CR4 检查** — SSE 相关控制位是否正确设置
3. **debugcon 标记法** — 用 `outb` 到端口 `0xE9` 在代码中插入标记，通过 `debug.log` 精确定位崩溃点

### 3. OS 引导的 SSE 初始化清单

在 x86_64 内核入口处，应执行以下初始化：

```
CR0: 清除 EM(bit 2), 清除 TS(bit 3), 设置 MP(bit 1)
CR4: 设置 OSFXSR(bit 9), 设置 OSXMMEXCPT(bit 10)
```

对应的汇编代码：

```asm
/* CR4: enable SSE state management */
movq %cr4, %rax
orq $((1 << 9) | (1 << 10)), %rax
movq %rax, %cr4

/* CR0: clear TS, clear EM, set MP */
movq %cr0, %rax
andq $(~(1 << 2)), %rax          /* clear EM */
orq $(1 << 1), %rax              /* set MP */
movq %rax, %cr0
clts                               /* clear TS */
```

### 4. QEMU "unexpected exit code: 0" 的含义

`isa-debug-exit` 设备的退出码公式为 `(value << 1) | 1`，永远是奇数（1, 3, 5...）。退出码 0 表示 **不是通过 isa-debug-exit 退出**，通常意味着 Triple Fault（配合 `-no-reboot` 标志）。

---

## 相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/mini/arch/x86_64/boot.S` | 在 `_start` 入口添加 CR4.OSFXSR/OSXMMEXCPT 设置 + `clts` |

---
