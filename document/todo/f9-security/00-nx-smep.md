# M1: NX + SMEP/SMAP

> CPU 硬件级安全特性。防止在用户态数据/栈上执行代码（NX），
> 防止内核意外访问用户态内存（SMEP/SMAP）。

## 任务清单

### T1: NX (No-Execute) 位

**文件**: `kernel/arch/x86_64/paging_config.hpp`（已有 FLAG_NX）

当前 ELF 加载已设置 NX 位（非可执行段）。验证和完善：

- [ ] 确认所有用户态栈页设置 FLAG_NX
- [ ] 确认 mmap PROT_EXEC 控制正确
- [ ] 确认内核页表也使用 NX（除了 .text 区域）
- [ ] 测试：执行栈上代码 → #GP/SIGSEGV

### T2: SMEP (Supervisor Mode Execution Prevention)

**文件**: `kernel/arch/x86_64/cpu_features.hpp`

CR4 bit 20 = SMEP。阻止内核态执行用户态页面：

```cpp
void enable_smep() {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 20);  // SMEP
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}
```

- [ ] 启动时检测 CPUID.07H:EBX bit 7 (SMEP support)
- [ ] 如果支持，设置 CR4.SMEP
- [ ] 确认所有内核代码页不在用户态区域
- [ ] QEMU 验证：内核尝试执行用户态地址 → #GP

### T3: SMAP (Supervisor Mode Access Prevention)

CR4 bit 21 = SMAP。阻止内核态读写用户态页面（除非通过 stac/clac）：

```cpp
// 临时允许内核访问用户态（syscall handler 中）
inline void stac() { __asm__ volatile("stac"); }
inline void clac() { __asm__ volatile("clac"); }
```

- [ ] 检测 CPUID.07H:EBX bit 20 (SMAP support)
- [ ] 设置 CR4.SMAP
- [ ] syscall 入口处 stac()，返回前 clac()
- [ ] copy_from_user / copy_to_user 使用 stac/clac 保护

## 产出物

- [ ] NX 位全面启用
- [ ] SMEP/SMAP 检测 + 启用
- [ ] stac/clac 用户态数据访问
- [ ] QEMU 验证
