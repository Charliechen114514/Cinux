---
title: SYSCALL RBX 覆写
---

# 024-02: SYSCALL 入口未恢复 RBX 导致 Shell 命令全部失效

## 一、问题现象

**症状**：Shell 正常启动并显示 prompt，键盘输入字符能正确回显，但输入 `echo hello` 后按回车，屏幕上没有输出 `hello`；输入 `clear` 也没有清屏效果。

```log
Cinux shell - type 'help' for commands
cinux> echo hello
cinux> clear
cinux>
```

**特征**：
- Shell 启动正常，prompt 显示正常
- 键盘输入字符能正确回显（sys_read + sys_write 基本通路工作）
- 所有命令均无效果——不是某个命令的 bug，而是共性问题

---

## 二、排查过程

### 第一步：添加调试信息定位故障层

在内核 `sys_read`、`sys_write`、`syscall_dispatch` 以及用户态 shell 主循环中添加 debug 输出：

- `syscall_dispatch`：打印每次 syscall 的编号和参数
- `sys_read`：打印每个读到的字符及其 hex 值
- `sys_write`：打印 fd、count、buffer 地址
- shell `main.cpp`：打印读到的 line、argc、argv[0]

### 第二步：实现用户态 printf

最初在 shell 中用内联代码格式化数字，导致 NULL 指针解耦崩溃（#PF at CR2=0x0）。于是实现了完整的用户态 `printf`（`user/libc/printf.cpp`），支持 `%s`、`%d`、`%u`、`%x`、`%p`、`%c`、`%%` 以及 `%l`/`%ll` 长度修饰符。

### 第三步：修复 kprintf 格式说明符

内核 `kprintf` 不支持 `%lu`/`%lx` 长度修饰符（只支持 `%u`/`%x`/`%d`/`%s`/`%c`/`%p`），导致 debug 输出打印为字面字符串。改为 `%u`/`%x` 后正确输出。

### 第四步：关键发现——`line='' len=1`

修复格式后得到的 debug trace：

```log
[READ] char=0x65 'e' total=1
[READ] returning 1 bytes
[WRITE] fd=1 count=1 buf=0x7FFFFEE5F first=0x65
e
...  （所有 10 个字符都正确读入并回显）
[READ] char=0xa '.' total=1
[READ] returning 1 bytes
[WRITE] fd=1 count=1 buf=0x400CB1 first=0xa

[WRITE] fd=1 count=20 buf=0x7FFFFECF0 first=0x5b
[DBG] line='' len=1
```

**关键异常**：用户输入了 "echo hello"（10 个字符），但 `line` 为空字符串，`len=1`。

### 第五步：反汇编定位根因

对 `user_shell` ELF 反汇编，发现编译器将 `read_line` 内联到 `shell_main` 中：

```asm
; 读循环核心：
400057: lea    rsi, [rsp+0xf]        ; &c
400061: call   sys_read               ; sys_read(0, &c, 1)
...
40008f: call   sys_write              ; write_buf(&c, 1)  ← 回显
400099: lea    rdx, [rbx+0x1]         ; rdx = pos + 1
40009d: mov    BYTE PTR [rsp+rbx+0x90], al  ; line[pos] = c
4000ad: mov    rbx, rdx               ; pos = pos + 1
```

`rbx` 存储的是 `pos`（当前写入位置）。逻辑上每次迭代 `rbx` 递增，10 个字符后 `rbx=10`。

**但每次 `sys_read`/`sys_write` 都通过 SYSCALL 进入内核。** 检查 `syscall.S` 发现：

```asm
# Step 5: Save return value in RBX
movq %rax, %rbx              # ← 用 RBX 暂存返回值！

# Step 6: Restore user state
movq 0(%rsp), %rax           ; user RSP
movq 8(%rsp), %rcx           ; user RIP
movq 16(%rsp), %r11          ; user RFLAGS
addq $96, %rsp               ; 释放整个 trap frame
movq %gs:8, %rsp             ; 切回用户栈
movq %rbx, %rax              ; 恢复返回值到 RAX
swapgs
sysretq
```

**根因确认**：

1. 用户的 `rbx`（= `pos`）在入口处被 `push` 到 trap frame（rsp+80）
2. 内核用 `movq %rax, %rbx` 把返回值暂存到 `rbx`，**覆盖了用户的 `rbx`**
3. 恢复用户状态时，**没有从 trap frame 恢复用户原始 `rbx`**
4. `addq $96, %rsp` 释放了整个 trap frame，原始 `rbx` 值彻底丢失
5. SYSRETQ 返回 Ring 3 时，`rbx` = syscall 返回值（通常为 1），而非用户的 `pos`

### 第六步：追踪实际执行流

每个字符的处理：

1. `sys_read` → SYSCALL → `rbx` 被覆盖为返回值 1
2. `sys_write` → SYSCALL → `rbx` 再次被覆盖为返回值 1
3. `lea rdx, [rbx+0x1]` → `rdx = 1 + 1 = 2`
4. `mov [rsp+rbx+0x90], al` → 写入 `line[1]`（而非 `line[pos]`）
5. `mov rbx, rdx` → `rbx = 2`

每个字符都写到 `line[1]`，前一个字符被覆盖。最终 `line[0]` 从未被写入，`line[1]` 保存最后一个字符。

按下回车时：

1. `sys_write("\n")` → `rbx` 被覆盖为 1
2. `mov [rsp+rbx+0x90], 0` → `line[1] = '\0'`
3. `mov rdx, rbx` → `rdx = 1`（即 `len=1`）

**结果**：`line = ""`（空字符串），`len = 1`。所有命令都无法匹配，表现就是 echo/clear 全部失效。

---

## 三、修复方案

### 核心思路

不再用 `rbx` 暂存返回值，改用 per-CPU GS scratch 区域的空闲 slot（`gs:16`）。

### 改动

**`kernel/arch/x86_64/syscall.S`**：

```asm
# Before (BUG):
movq %rax, %rbx              # 暂存返回值，破坏了用户 rbx
...
addq $96, %rsp               # 释放 trap frame（用户 rbx 丢失）
...
movq %rbx, %rax              # 恢复返回值

# After (FIX):
movq %rax, %gs:16            # 暂存返回值到 GS scratch slot 2
...
movq 80(%rsp), %rbx          # ← 从 trap frame 恢复用户 rbx！
addq $96, %rsp               # 释放 trap frame
...
movq %gs:16, %rax            # 从 scratch 恢复返回值
```

完整 diff：

```asm
    call syscall_dispatch
    addq $8, %rsp

-   movq %rax, %rbx               # 破坏用户 RBX
+   movq %rax, %gs:16             # 保存到 GS scratch

    movq 0(%rsp), %rax
    movq %rax, %gs:8
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
+   movq 80(%rsp), %rbx           # 恢复用户 RBX

    addq $96, %rsp
    movq %gs:8, %rsp
-   movq %rbx, %rax
+   movq %gs:16, %rax             # 恢复返回值
    swapgs
    sysretq
```

### 为什么 gs:16 是安全的？

GS base 页在 `launch_first_user()` 中分配（一个完整的 4KB 页）：

```cpp
auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
gs_virt[0] = kernel_rsp0;   // gs:0  — 内核栈指针
gs_virt[1] = 0;              // gs:8  — 临时存储 user RSP
// gs:16 空闲，可用作 scratch
```

---

## 四、修复后验证

Shell 启动后输入 `echo hello` 正确输出 `hello`，输入 `clear` 正确清屏。

---

## 五、经验教训

### 1. x86_64 ABI callee-saved 寄存器不可破坏

`rbx`、`rbp`、`r12`-`r15` 是 callee-saved 寄存器。SYSCALL 入口/出口相当于一次函数调用，必须保证这些寄存器在返回用户态时与进入时一致。**哪怕内核内部需要暂存数据，也必须先保存、后恢复。**

### 2. SYSCALL/SYSRETQ 只自动保存/恢复 RCX 和 R11

- `RCX` 被硬件用来保存返回 RIP（SYSRETQ 从 RCX 加载 RIP）
- `R11` 被硬件用来保存 RFLAGS（SYSRETQ 从 R11 加载 RFLAGS）
- **所有其他寄存器必须由软件保存/恢复**

### 3. 反汇编是定位寄存器破坏问题的终极手段

当怀疑寄存器被 clobber 时，反汇编用户态和内核态的入口/出口代码是唯一的确定方法。编译器优化（如将 `pos` 分配到 `rbx`）只有在看到汇编后才能确认。

### 4. "所有命令都失效"指向基础设施问题

如果每个命令都有问题，不要逐个命令排查，应怀疑 syscall 机制本身、字符串比较、内存布局等底层共性问题。

---

## 相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/arch/x86_64/syscall.S` | 返回值暂存从 `rbx` 改为 `gs:16`，恢复用户 `rbx` |
| `user/libc/printf.hpp` | 新增用户态 printf 声明 |
| `user/libc/printf.cpp` | 新增用户态 printf 实现 |
| `user/CMakeLists.txt` | 将 `printf.cpp` 加入 `user_libc` |
