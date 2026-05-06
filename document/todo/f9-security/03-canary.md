# M4: Stack Canary

> 编译器级别的栈溢出保护。通过 -fstack-protector 编译选项启用。

## 任务清单

### T1: 编译器配置

**文件**: `kernel/CMakeLists.txt`

```cmake
# 为内核和用户态程序启用 stack protector
target_compile_options(big_kernel PRIVATE -fstack-protector-strong)
```

- [ ] 内核启用 -fstack-protector-strong
- [ ] 用户态程序同样启用

### T2: __stack_chk_fail 实现

**文件**: `kernel/lib/stack_protector.cpp`

```cpp
extern "C" __attribute__((noreturn))
void __stack_chk_fail() {
    kpanic("Stack smashing detected!");
}
```

- [ ] __stack_chk_fail — kernel panic
- [ ] 用户态版 — kill(SIGABRT)

### T3: Canary 值初始化

- [ ] 内核 canary = g_random.next64()（使用 M2 的 KRandom）
- [ ] 每个进程独立的 canary（存入 TCB）
- [ ] GS 段寄存器指向 canary（x86_64 __tls_get_addr 模式）

## 产出物

- [ ] CMake -fstack-protector-strong
- [ ] __stack_chk_fail 实现
- [ ] 随机 canary 初始化
