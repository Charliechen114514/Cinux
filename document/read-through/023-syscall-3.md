# 023-3 Read-through: usermode 改造 + 用户态编译 + FPU/SSE + 测试

## 概览

本文覆盖 023 tag 的剩余部分：usermode.cpp 的改造（GS base 设置、ABI 栈对齐、嵌入用户程序）、boot.S 的 FPU/SSE 初始化、用户态编译基础设施（user/ 目录）、以及测试代码。这些内容横跨硬件初始化、编译系统、和端到端验证三个维度。

关键设计决策：GS 数据页替代直接全局变量存内核栈、编译期 static_assert 验证栈对齐、用户态程序通过 ld -r -b binary 嵌入内核。

## 架构图

```
boot.S                   usermode.cpp                user/
┌──────────────┐    ┌────────────────────┐    ┌───────────────────┐
│ cli          │    │ usermode_init()    │    │ programs/hello.cpp│
│ movq rsp     │    │   GS page alloc    │    │   _start()        │
│ xorq rbp     │    │   wrmsr GS_BASE    │    │   sys_write(1,msg)│
│ ──FPU init── │    │                    │    │   sys_exit(0)     │
│  CR4 OSFXSR  │    │ launch_first_user()│    ├───────────────────┤
│  CR0 clear EM│    │   alloc code page  │    │ libc/syscall.cpp  │
│  clts        │    │   copy binary      │    │   _syscall1()     │
│              │    │   alloc stack pages│    │   _syscall3()     │
│ BSS clear    │    │   copy PDPT entries│    ├───────────────────┤
│ ctors        │    │   set GS page      │    │ linker.ld         │
│ kernel_main  │    │   RSP -= 8 (ABI)   │    │   VMA=0x400000    │
└──────────────┘    │   jump_to_usermode │    ├───────────────────┤
                    └────────────────────┘    │ CMakeLists.txt    │
                                              │   ELF→bin→embed.o │
                                              └───────────────────┘
```

## 代码精讲

### boot.S -- FPU/SSE 初始化

```asm
    movq  $__kernel_stack_top, %rsp
    xorq  %rbp, %rbp

    /* FPU init -- AFTER stack setup */
    movq  %cr4, %rax
    orq   $((1 << 9) | (1 << 10)), %rax
    movq  %rax, %cr4

    movq  %cr0, %rax
    andq  $(~(1 << 2)), %rax
    orq   $(1 << 1), %rax
    movq  %rax, %cr0
    clts
```

FPU 初始化插入在栈设置之后。CR4 的 bit 9 是 OSFXSR（告诉 CPU 操作系统支持 FXSAVE/FXRSTOR），bit 10 是 OSXMMEXCPT（允许 SIMD 异常通过中断报告）。CR0 清除 bit 2（EM，不模拟协处理器），设置 bit 1（MP，配合 TS 位使用），clts 清除 TS 位（否则第一条 SSE 指令会触发 #NM）。

这里有一个至关重要的顺序约束——这三段代码必须出现在 cli + movq rsp 之后。因为 mini kernel 通过检查大内核入口前三个字节（FA 48 BC）来判断是否为真实内核。如果把 FPU init 的 `movq %cr4, %rax`（字节码 0F 20）插到 cli 和 movq rsp 之间，字节码就变成了 FA 0F 20，mini kernel 不认，所有大内核测试都会被跳过。笔者在这里被坑了半天——表面上是"测试全跳过"，实际上是 boot.S 指令顺序的问题。

### usermode.cpp -- GS base 与栈对齐

```cpp
namespace {
constexpr uint64_t USER_STACK_TOP = 0x7FFFFF000;
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");
constexpr uint64_t USER_STACK_PAGES = 4;
}
```

USER_STACK_TOP 和 USER_ABI_RSP_OFFSET 从 usermode.hpp 移到了 cpp 的匿名命名空间——这些是实现细节，不需要暴露给其他编译单元。static_assert 在编译期验证栈对齐：0x7FFFFF000 - 8 = 0x7FFFFEFF8，对 16 取模等于 8，满足 x86-64 SysV ABI 的要求。

```cpp
void usermode_init() {
    usermode_init_asm();
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    uint64_t gs_phys = g_pmm.alloc_page();
    auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
    gs_virt[0] = 0;  // kernel_rsp -- filled later
    gs_virt[1] = 0;
    write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);
}
```

usermode_init 在 usermode_init_asm（配置 STAR/EFER）之后，分配 GS 数据页并写入 KERNEL_GS_BASE MSR。注意 gs_virt[0] 初始为 0——内核栈指针会在后续的 launch_first_user 或调度器切换时填入。

### launch_first_user -- 嵌入用户程序

launch_first_user 的改动主要是把手写的 kUserCode[] 字节数组替换为嵌入的用户程序二进制。通过 extern 声明 `_binary_hello_bin_start` 和 `_binary_hello_bin_end` 符号，计算大小后拷贝到用户代码页。跳转时传入 `USER_STACK_TOP - USER_ABI_RSP_OFFSET` 而不是直接的 USER_STACK_TOP。

### user/libc/syscall.cpp -- 用户态 SYSCALL 封装

```cpp
static inline int64_t _syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}
```

_syscall3 是用户态的三参数 syscall 封装。内联汇编把 syscall 号放入 RAX（"a"约束），三个参数分别放入 RDI（"D"）、RSI（"S"）、RDX（"d"）。clobber 列表包含 rcx（SYSCALL 自动覆盖为 RIP）、r11（SYSCALL 自动覆盖为 RFLAGS）、memory（syscall 可能修改内存）。基于此封装 sys_write 和 sys_read。sys_exit 使用 _syscall1（单参数版本），并在之后标记 `__builtin_unreachable()`——因为 sys_exit 不返回。

### user/programs/hello.cpp -- 第一个用户态 C++ 程序

```cpp
extern "C" void _start() {
    const char msg[] = "[USER] Hello from Ring 3!\n";
    sys_write(1, msg, 26);
    sys_exit(0);
}
```

整个程序只有三行。入口函数必须是 `_start`（链接脚本 ENTRY 指定），用 extern "C" 避免 name mangling。这个程序就是之前讨论的那个"GCC 会用 movaps 优化字符串初始化"的罪魁祸首——正是这个程序暴露了 FPU/SSE 和栈对齐的问题。

### user/linker.ld -- 用户态链接脚本

链接脚本设置 VMA=0x400000，和内核的 USER_ENTRY_BASE 一致。.text.start 段优先排列，保证 _start 函数在二进制文件的最开头。所有调试信息段（.comment、.note、.eh_frame）被 /DISCARD/ 掉以减小体积。

### 测试覆盖

测试分为两层。Host 端单元测试（test/unit/test_syscall.cpp）用 mock 重新实现内核逻辑，测试分发表注册/分发、sys_write 参数验证、sys_exit 状态转换、STAR MSR 计算等纯逻辑。内核端集成测试（kernel/test/test_syscall.cpp）在 QEMU 里运行，直接读回 MSR 值、注册真实 handler 并 dispatch、验证 sys_write 的 fd 检查和地址验证。

## 设计决策

### 决策：用 ld -r -b binary 嵌入用户程序

**问题**：需要在内核镜像中包含用户程序二进制，运行时拷贝到用户代码页。

**本项目的做法**：CMake 编译用户程序为 ELF → objcopy 转 flat binary → ld -r -b binary 包装为 .o → 链接到内核。生成 _binary_hello_bin_start/end 符号。

**备选方案**：用 .incbin 汇编伪指令在汇编文件中包含二进制。

**为什么不选备选方案**：incbin 需要手动管理路径和依赖关系，ld -r 方案由 CMake 自动跟踪依赖，增量编译更正确。

### 决策：static_assert 验证栈对齐

**问题**：ABI 栈对齐是很容易被忽视的细节，运行时错误表现为 movaps #GP，极难定位。

**本项目的做法**：编译期 static_assert 检查 (USER_STACK_TOP - 8) % 16 == 8。

**备选方案**：运行时 assert 或文档约束。

**为什么不选备选方案**：运行时错误太晚——系统可能直接 triple fault。编译期错误是最早最安全的检查点。

## 扩展方向

- ⭐ 修改 hello.cpp 让它接受一个参数（通过 jump_to_usermode 的 arg 参数传入），打印不同的消息
- ⭐⭐ 实现用户态 sys_read，从键盘读取输入
- ⭐⭐ 用 CMake 的 generator expression 支持多个用户程序并行编译和切换
- ⭐⭐⭐ 实现 copy_from_user/copy_to_user 安全内存访问机制

## 参考资料

- Intel SDM Vol.3A §2.5: CR0/CR4 控制寄存器位定义（OSFXSR, OSXMMEXCPT, EM, MP, TS）
- x86-64 SysV ABI: 函数入口 RSP 对齐要求（RSP ≡ 8 mod 16）
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
- Linux kernel entry_64.S: https://www.kernel.org/doc/Documentation/x86/entry_64.txt
