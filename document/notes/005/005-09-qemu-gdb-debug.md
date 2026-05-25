---
title: QEMU GDB 调试
---

# 005-09: QEMU GDB 调试

## 概述

QEMU 内置 GDB stub 功能，允许开发者通过 GDB 远程调试虚拟机中的代码。Cinux 使用 QEMU 的 `-s` 和 `-S` 参数实现内核调试。

---

## 1. QEMU GDB 参数

### 核心参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-s` | 启动 GDB stub，监听 TCP 端口 1234 | 关闭 |
| `-S` | 启动时暂停 CPU，等待 GDB 连接 | 直接启动 |

### 完整启动命令

```bash
qemu-system-x86_64 \
    -drive file=build/cinux.img,format=raw \
    -s \                    # GDB stub on :1234
    -S \                    # 启动时暂停
    -serial stdio \         # 串口输出到终端
    -no-reboot \            # 禁用自动重启
    -no-shutdown            # panic 时不退出
```

### CMake 配置

来源：[`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake#L33-L36)

```cmake
set(QEMU_DEBUG_FLAGS
    -s      # GDB stub on localhost:1234
    -S      # Freeze at startup
)
```

---

## 2. GDB Remote 协议

### 连接流程

```
┌─────────────────────────────────────────────────────────────┐
│                    GDB Remote 连接流程                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. QEMU 启动                                               │
│     qemu-system-x86_64 -s -S disk.img                      │
│            │                                                │
│            ▼                                                │
│     QEMU GDB stub 监听 :1234                                │
│                                                             │
│  2. GDB 连接                                                │
│     gdb kernel.elf                                          │
│     (gdb) target remote :1234                               │
│            │                                                │
│            └──────────────► TCP 连接建立                     │
│                                                             │
│  3. GDB 发送命令                                            │
│     - 读取寄存器: $g               (GDB → QEMU)            │
│     - 读取内存: $m addr,length    (GDB → QEMU)            │
│     - 写入内存: $M addr,length:... (GDB → QEMU)            │
│     - 单步执行: $s                (GDB → QEMU)            │
│     - 继续执行: $c                (GDB → QEMU)            │
│            │                                                │
│            ▼                                                │
│     QEMU 执行命令，返回结果                                  │
│                                                             │
│  4. 断点处理                                                │
│     - GDB 设置断点: $Z0,addr,kind  (插入软件断点)          │
│     - QEMU 执行到断点暂停                                   │
│     - QEMU 通知 GDB: $S05          (SIGTRAP)               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 协议数据包示例

```
GDB → QEMU: $g#7a          # 读取所有寄存器
QEMU → GDB: xxxxxxxx...    # 寄存器值（16进制）

GDB → QEMU: $m7c00,2#xx    # 读取 0x7c00 开始的 2 字节
QEMU → GDB: xxxxxx         # 内存内容

GDB → QEMU: $Z0,7c00,1#xx  # 在 0x7c00 设置断点
QEMU → GDB: $OK#9a         # 成功

GDB → QEMU: $s#xx          # 单步执行
QEMU → GDB: $S05#xx        # 停止，SIGTRAP

GDB → QEMU: $c#xx          # 继续执行
QEMU → GDB: $S05#xx        # 遇到断点，SIGTRAP
```

---

## 3. 常用 GDB 命令

### 基础命令

| 命令 | 缩写 | 说明 |
|------|------|------|
| `target remote :1234` | - | 连接到 QEMU GDB stub |
| `file kernel.elf` | - | 加载符号文件 |
| `break main` | `b main` | 设置断点 |
| `continue` | `c` | 继续执行 |
| `next` | `n` | 单步跳过（源码级） |
| `step` | `s` | 单步进入（源码级） |
| `nexti` | `ni` | 单步跳过（指令级） |
| `stepi` | `si` | 单步进入（指令级） |
| `finish` | `fin` | 执行到当前函数返回 |
| `backtrace` | `bt` | 显示调用栈 |
| `frame N` | `f N` | 切换到第 N 层栈帧 |

### 信息查看

| 命令 | 说明 |
|------|------|
| `info registers` | 显示所有寄存器 |
| `info registers rip` | 显示特定寄存器 |
| `x/10i $pc` | 反汇编当前指令（10 条） |
| `x/10x 0xaddress` | 以十六进制显示内存 |
| `x/10s 0xaddress` | 以字符串显示内存 |
| `disassemble` | 反汇编当前函数 |

### 断点管理

| 命令 | 说明 |
|------|------|
| `break *0xaddress` | 在地址设置断点 |
| `break main` | 在函数入口设置断点 |
| `info breakpoints` | 显示所有断点 |
| `delete N` | 删除第 N 个断点 |
| `disable N` | 禁用第 N 个断点 |
| `enable N` | 启用第 N 个断点 |
| `watch *0xaddress` | 设置观察点（内存变化时触发） |

---

## 4. 调试会话示例

### 示例 1：调试内核入口

```bash
# Terminal 1: 启动 QEMU
cd build && make run-debug
# QEMU 启动并暂停

# Terminal 2: 启动 GDB
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
Remote debugging using :1234
0x00007c00 in ?? ()
(gdb) break *0xFFFFFFFF80020000
Breakpoint 1 at 0xffffffff80020000
(gdb) continue
Continuing.

Breakpoint 1, 0xffffffff80020000 in _start ()
(gdb) x/5i $pc
=> 0xffffffff80020000 <_start>: cli
   0xffffffff80020001 <_start+1>: movb $0x31, %al
   0xffffffff80020003 <_start+3>: outb %al, $0xe9
   0xffffffff80020005 <_start+5>: movq $__mini_stack_top, %rsp
   0xffffffff8002000c <_start+12>: xorq %rbp, %rbp
```

### 示例 2：调试 C++ 函数

```gdb
(gdb) break mini_kernel_main
Breakpoint 2 at 0xffffffff800201a0
(gdb) continue
Continuing.

Breakpoint 2, mini_kernel_main (boot_info_addr=18446744071562067968)
    at kernel/mini/test/main_test.cpp:20
20      BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
(gdb) list
15      using cinux::mini::lib::kdebugf;
16
17      extern "C" {
18      extern uint64_t __boot_info_ptr;
19      void run_cpp_tests();
20      }
21
22      extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
23      BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
24      (void)boot_info_addr;
25      (void)boot_info;
(gdb) print __boot_info_ptr
$1 = 28672
(gdb) x/1gx &__boot_info_ptr
0xffffffff800226e8 <__boot_info_ptr>: 0x0000000000007000
```

---

## 5. 模式切换调试

### 问题：保护模式 → 长模式切换

GDB 在模式切换时可能丢失符号或无法正确单步。

**解决方案**：

```gdb
# 在已知稳定的地址设置断点
(gdb) break *0x7c00     # 实模式入口
(gdb) break *0x8000     # 保护模式入口
(gdb) break *0xFFFFFFFF80020000  # 长模式入口

# 使用 continue 而非 step 跨越模式切换
(gdb) continue
```

---

## 6. 常见问题

### Q1: GDB 显示 "Cannot access memory at address"

**原因**：地址未映射或页表未设置

**解决**：
```gdb
# 检查 CR3 寄存器（页表基址）
(gdb) info registers cr3
# 检查是否在正确的地址空间
```

### Q2: 单步执行跳到错误位置

**原因**：优化导致代码重排

**解决**：
```bash
# 使用 Debug 构建
cmake -DCMAKE_BUILD_TYPE=Debug -B build
```

### Q3: 符号未加载

**原因**：ELF 文件路径不匹配

**解决**：
```gdb
(gdb) file build/kernel/mini/mini_kernel
(gdb) add-symbol-file build/boot/stage2
```

---

## 7. QEMU 与 GDB 版本兼容性

| QEMU 版本 | GDB 版本 | 兼容性 |
|-----------|----------|--------|
| ≤ 8.0 | ≤ 13 | 完全兼容 |
| 8.1 - 9.0 | 14 - 15 | 完全兼容 |
| ≥ 9.0 | ≥ 16 | 需要 `-gdb-set architecture i386:x86-64` |

### GDB 17.x 兼容性修复

```gdb
# .vscode/gdbinit
set architecture i386:x86-64
set disassembly-flavor intel
```

---

## 8. 参考资源

### 官方文档
- [QEMU GDB documentation](https://www.qemu.org/docs/master/system/gdb.html) - 官方 GDB 使用文档
- [GDB Remote Serial Protocol](https://sourceware.org/gdb/current/onlinedocs/gdb/Remote-Protocol.html) - 协议规范

### 社区资源
- [OSDev Wiki: GDB](https://wiki.osdev.org/GDB) - OS 开发者社区指南
- [Debugging long mode kernel](https://forum.osdev.org/viewtopic.php?t=30285) - 长模式调试经验

### 相关链接
- [005-08: VSCode 调试配置](005-08-vscode-debug-setup.md) - VSCode 集成配置
- [005-11: 调试工作流总结](005-11-debug-workflows.md) - 完整调试流程
