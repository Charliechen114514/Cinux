# 021 · 大内核入口魔数检查失败，测试无法进入 big kernel

## 现象

`make run-kernel-test` 输出：

```
=== Mini kernel tests completed ===
=== MINI KERNEL TESTS PASSED ===

=== Loaded ELF is not a real kernel, exiting ===
[100%] Built target run-kernel-test
```

Mini kernel 测试全部通过，但 big kernel 测试从未执行，直接退出。

## 分析

Mini kernel 在跳转 big kernel 之前会校验入口字节（`kernel/mini/test/main_test.cpp`）：

```cpp
// 只接受一种编码：
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) && (code[2] == 0xBC);
```

`_start` 前两条指令是 `cli` + `movq $__kernel_stack_top, %rsp`。GNU assembler 对后者有两种合法编码：

| 编码 | 机器码 | 条件 |
|------|--------|------|
| `REX.W MOV r64, imm64` | `48 BC <8字节立即数>` | 无条件 64 位 |
| `REX.W MOV r/m64, imm32` | `48 C7 C4 <4字节立即数>` | 立即数可符号扩展到 64 位 |

关键：当 `__kernel_stack_top` 的低 32 位符号扩展后恰好等于 64 位值时，assembler 会选择更短的 `imm32` 编码。

添加 `sync.cpp` 后 BSS 段增长，`__kernel_stack_top` 从一个需要 `imm64` 编码的地址变成了可以用 `imm32` 符号扩展表示的地址：

```
# 实际二进制入口字节（添加 sync.cpp 后）：
FA 48 C7 C4 00 00 02 81 ...
# ^cli  ^--- mov rsp, sign-ext imm32 ---^
```

旧检查只认 `48 BC`，遇到 `48 C7` 就判为"不是真内核"。

## 修复

放宽魔数检查，同时接受两种编码：

```cpp
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                      (code[2] == 0xC7 || code[2] == 0xBC);
```

## 教训

x86-64 的 `mov` 指令有等价的不同编码形式，assembler 会根据立即数范围自动选择最短编码。任何基于机器码字节模式的检查都应覆盖所有合法编码，而非只匹配某一种。BSS 大小的变化就能改变链接地址，进而改变 assembler 的编码选择。
