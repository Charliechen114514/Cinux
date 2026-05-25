---
title: FPU/SSE 排查
---

# 023_syscall FPU/SSE 与用户态编译排查笔记

## 问题 1：用户态程序 #GP — 编译器生成 SSE 指令

### 现象

用户态 C++ 程序在 Ring 3 执行时触发 #GP：

```
RIP   = 0x0000000000400019
RSP   = 0x00000007FFFFEFD8
movaps XMMWORD PTR [rsp],xmm0  ← #GP
```

### 根因

GCC 将 `const char msg[] = "..."` 的初始化优化为 SSE `movdqa`/`movaps` 指令。`movaps` 要求目标地址 16 字节对齐，但 RSP 未对齐。

反汇编确认：
```asm
400000: sub    rsp, 0x28
...
400019: movaps XMMWORD PTR [rsp], xmm0   ← 崩溃点
```

### 解决路径

有两个方案：

**方案 A**：禁用 SSE（加 `-mgeneral-regs-only`）—— 临时方案
**方案 B**：在内核启用 FPU/SSE 支持 —— 正确方案

选择方案 B，因为用户态程序理应能使用 SSE。

---

## 问题 2：FPU/SSE 启用后仍然 #GP — 栈对齐

### 现象

FPU 已在 boot.S 中通过 CR0/CR4 启用，但 `movaps` 仍然触发 #GP。

### 根因

不是 FPU 未启用，而是栈未满足 x86_64 SysV ABI 对齐要求：

- x86_64 ABI：函数入口 RSP ≡ **8 mod 16**（模拟 `call` 指令压入 8 字节返回地址）
- 我们的 `USER_STACK_TOP = 0x7FFFFF000`，是 **0 mod 16**
- 编译器假设 RSP ≡ 8 mod 16，生成 `sub rsp, 0x28` 后期望 RSP 对齐
- 0x7FFFFF000 - 0x28 = 0x7FFFFEFD8，**8 mod 16 ≠ 0**，`movaps` 对齐失败

### 修复

跳转前 RSP 减 8，并用 `static_assert` 编译期检查：

```cpp
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");
```

调用处：

```cpp
jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

---

## 问题 3：boot.S 指令顺序导致大内核测试无法启动

### 现象

`make run-kernel-test` 输出 "Loaded ELF is not a real kernel, exiting"，169 个大内核测试全部跳过。

### 根因

mini kernel 通过检查大内核入口前 3 字节验证是否为真实内核：

```cpp
// kernel/mini/test/main_test.cpp
bool is_real_kernel = (code[0] == 0xFA) &&       // cli
                      (code[1] == 0x48) &&        // REX.W
                      (code[2] == 0xC7 || code[2] == 0xBC);  // mov rsp, imm
```

原始 boot.S 顺序：`cli` → `movq $__kernel_stack_top, %rsp`（字节 FA 48 BC ✓）

加入 FPU 初始化后变为：`cli` → `movq %cr4, %rax`（字节 FA 0F 20 ✗）

### 修复

把 FPU 初始化移到栈设置之后，保持 `cli` + `mov rsp, imm` 为前两条指令：

```asm
_start:
    cli
    movq  $__kernel_stack_top, %rsp   # FA 48 BC — mini kernel 验证点
    xorq  %rbp, %rbp
    # FPU init follows...
    movq  %cr4, %rax
    orq   $((1 << 9) | (1 << 10)), %rax
    ...
```

---

## FPU 支持改动总览

| 文件 | 改动 |
|------|------|
| `kernel/arch/x86_64/boot.S` | CR0/CR4 初始化（OSFXSR + OSXMMEXCPT + 清 EM/TS） |
| `kernel/proc/process.hpp` | Task 结构体增加 `alignas(16) uint8_t fpu_state[512]` |
| `kernel/proc/process.cpp` | TaskBuilder::build() 中 fninit + fxsave 初始化 FPU 状态 |
| `kernel/proc/scheduler.cpp` | schedule()/exit_current()/run_first() 中 fxsave/fxrstor |
| `kernel/arch/x86_64/usermode.hpp` | USER_ABI_RSP_OFFSET + static_assert |
| `kernel/arch/x86_64/usermode.cpp` | jump_to_usermode 传 ABI 对齐的 RSP |

## 用户态编译基础设施

| 文件 | 作用 |
|------|------|
| `user/CMakeLists.txt` | 编译用户态程序 → objcopy flat binary → ld -r -b binary 嵌入 |
| `user/linker.ld` | 用户态链接脚本，VMA=0x400000 |
| `user/programs/hello.cpp` | C++ 写的 Hello 程序，替代手写字节码 |

关键编译标志：`-mcmodel=small`（用户态在低 2GB），`-fno-pie -nostdlib -static`。
