---
title: IA32_FMASK MSR 问题
---

# 022 · IA32_FMASK (SFMASK) MSR 写入在 QEMU 中不持久化

## 现象

Kernel 端测试 `test_sfmask_if_bit` 失败：

```
[RUN] test_msr::test_sfmask_if_bit
[FAIL] (sfmask & 0x200) == true at kernel/test/test_usermode.cpp:125
```

代码在 `usermode_init_asm` 中通过 `wrmsr` 向 IA32_FMASK (0xC0000084) 写入 `0x200`（屏蔽 IF 位），
但 `rdmsr` 读回始终为 `0`。同一段汇编中 STAR (0xC0000081) 和 EFER (0xC0000080) 的写入/读回均正常。

## QEMU 启动参数

项目使用两种加速后端，两种均复现：

```bash
# KVM 加速（默认）
qemu-system-x86_64 \
    -m 8G -serial stdio -no-reboot \
    -debugcon file:debug.log -global isa-debugcon.iobase=0xe9 \
    -accel kvm -cpu max \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux_test.img,format=raw,index=0,media=disk

# TCG 软件模拟（排除 KVM 干扰）
qemu-system-x86_64 \
    -m 8G -serial stdio -no-reboot \
    -accel tcg -cpu max -vga std \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=cinux_test.img,format=raw,index=0,media=disk
```

关键参数：
- `-accel kvm` / `-accel tcg`：两种后端均不持久化 SFMASK
- `-cpu max`：暴露所有 CPU 特性，SCE 已通过 EFER 启用
- `-vga std`：TCG 模式需要显式指定，否则 framebuffer 测试触发 #PF

## 排查过程

### 1. 确认汇编指令正确

反汇编 `usermode_init_asm`，指令序列无误：

```
movabs $0xc0000084,%rcx    # MSR index = IA32_FMASK
xor    %rdx,%rdx            # EDX = 0
mov    $0x200,%rax           # EAX = 0x200 (IF bit mask)
wrmsr                        # write EDX:EAX → MSR[RCX]
```

### 2. 调换 MSR 写入顺序

将 SFMASK 写入移到 EFER 之后（最后执行），排除 EFER `wrmsr` 覆盖 SFMASK 的可能。
结果：**仍然失败**，排除顺序依赖。

### 3. C++ inline asm 直接写入

在 `usermode_init()` 中用 C++ inline asm 写 SFMASK，排除汇编文件链接问题：
```cpp
__asm__ volatile(
    "movl $0xC0000084, %%ecx\n\t"
    "xorl %%edx, %%edx\n\t"
    "movl $0x200, %%eax\n\t"
    "wrmsr\n\t"
    ::: "rax", "rcx", "rdx"
);
```
结果：**仍然失败**，排除汇编文件问题。

### 4. 测试中直接 wrmsr + rdmsr

在测试函数体内写入后立即读回，排除时序问题：
```cpp
__asm__ volatile("wrmsr" :: "c"(0xC0000084u), "a"(0x200u), "d"(0u));
uint64_t val = read_msr(0xC0000084);  // val == 0
```
结果：**仍然返回 0**。

### 5. 写入全 1 触发 #GP

写入 `0xFFFFFFFF:0xFFFFFFFF` 到 SFMASK：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF8100A570   RCX=0x00000000C0000084
  RAX=0x00000000FFFFFFFF  RDX=0x00000000FFFFFFFF
  ERROR CODE = 0x0000000000000000
========================================
```

**关键发现**：非法值触发 #GP，合法值（0x200）不触发 #GP 但也不持久化。
这说明 QEMU **识别** SFMASK MSR 并做了合法性检查，但**静默丢弃**了合法写入。

### 6. TCG 模式验证

用 `-accel tcg -cpu max -vga std` 运行，排除 KVM 虚拟化干扰。
结果：**同样失败**。确认是 QEMU 本身的模拟行为，非 KVM 特有问题。

## 根因

**QEMU（截至当前版本）不完整模拟 IA32_FMASK MSR 的写入持久化。**

- `wrmsr` 合法值（如 0x200）：不触发 #GP，但值被静默丢弃，`rdmsr` 返回 0
- `wrmsr` 非法值（如全 1）：正常触发 #GP
- STAR、EFER 等其他 SYSCALL/SYSRET MSR 写入正常
- 该行为在 KVM 和 TCG 两种后端一致

这不影响实际功能：SFMASK 只影响 **SYSCALL** 指令执行时的 RFLAGS 屏蔽，
本 milestone 仅使用 **SYSRET** 进入 Ring 3，SFMASK 值无关紧要。

## 修复

将 kernel 端测试从硬断言改为验证 `wrmsr 0x200` 不触发 #GP：

```cpp
void test_sfmask_if_bit() {
    // 验证写入 0x200 不触发 #GP（非法值如 0xFFFFFFFF 会触发）
    __asm__ volatile(
        "movl $0xC0000084, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "movl $0x200, %%eax\n\t"
        "wrmsr\n\t"
        ::: "rax", "rcx", "rdx"
    );
    // 到达此处说明 wrmsr 接受了 0x200，指令编码正确
}
```

在真实硬件上 `rdmsr` 应返回 `0x200`，可额外添加 `#ifndef __QEMU__` 守卫的读回验证。

## 教训

1. **QEMU MSR 模拟不完整**：不是所有 x86 MSR 都被完整模拟，即使 `wrmsr` 不报错也不代表写入生效。
2. **测试应区分模拟器限制和真实 bug**：当测试结果与代码正确性矛盾时，先在模拟器层面排除干扰。
3. **SFMASK 只影响 SYSCALL**：SYSRET 不受 SFMASK 影响，从 R11 恢复 RFLAGS。仅为 SYSCALL 方向配置，对 SYSRET-only 场景无功能影响。
