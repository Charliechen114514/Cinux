# 004 加载小内核（构建系统与验证篇） -- flat binary 生成与完整构建验证

> 本章完成后的可见效果：QEMU debugcon 日志中出现 `OPL` 字符序列（O=disk read OK, P=Protected Mode, L=Long Mode）
>
> 前置要求：已完成 [004B-2 磁盘读取升级](004-boot-load-mini-kernel-b-2.md)

---

## 导语

前两篇我们定义了 BootInfo 结构体，把磁盘读取从 8 扇区扩展到 832 扇区。现在进入本章的收尾工作：更新小内核的构建系统配合 flat binary 方案、确认 Protected Mode 无操作设计、以及完整构建验证。这一篇涉及的主要文件是 CMakeLists.txt、linker.ld 和 build_image.sh，核心变更是从 ELF 加载切换到 flat binary 加载，以及加入内核大小限制检查。

---

## 一、概念精讲

### 1.1 Flat Binary 生成流程

我们的内核最终以 flat binary 格式加载，但编译链的中间产物仍然是 ELF。整个流程分三步：第一步，C++ 源码编译成目标文件（.o），链接器根据 linker script 把所有目标文件合并成一个 ELF 可执行文件——链接器本身需要 ELF 格式来工作（解析符号、处理重定位、计算地址）。第二步，用 `objcopy -O binary` 把 ELF 转成 flat binary——这个工具剥离所有 ELF header、section header table、符号表、重定位信息，只保留段的原始二进制内容。第三步，build_image.sh 用 dd 把 flat binary 写入磁盘镜像的 LBA 16 位置。

`objcopy -O binary` 的行为值得深入理解。它只输出 ELF 文件中 LMA（Load Memory Address）最低的那个段及其之后的段的内容。输出文件的第一个字节就是 LMA 最低段的第一个字节。如果 linker script 里 `.text.start` 段的 LMA 是 0x20000（最低），那 flat binary 的第一个字节就是 `_start` 的第一条指令。如果排序不对（比如某个 `.rodata` 段的 LMA 恰好更低），flat binary 可能以数据开头而不是代码开头，bootloader 跳过去就会执行乱码。

这就是为什么链接脚本里 `*(.text.start)` 必须排在 `.text` 通配符之前——确保 `_start` 的代码是输出文件的第一段内容。这个排序在 ELF 格式里无所谓（ELF 有 entry point 字段告诉加载器从哪里开始），但在 flat binary 里至关重要（没有 header，默认从第一个字节开始执行）。

### 1.2 Protected Mode 为什么什么都不做

你可能会问：既然内核已经加载完成了，为什么不直接在 Protected Mode 跳转？答案是我们需要进入 Long Mode 才能执行 64 位代码。小内核是用 `-mcmodel=large -m64` 编译的 64 位代码，在 32 位 Protected Mode 下无法运行。所以即使加载完成了，也必须继续完成页表设置、Long Mode 初始化，然后在 64 位模式下跳转——这是 004C 的内容，不是 004B。

另一个原因是内存布局一致性。小内核已经通过 flat binary 方式加载到了 0x20000——它在磁盘镜像上的位置和在内存里的位置完全对应，不需要任何重定位或搬移。如果 Protected Mode 里要做 ELF 解析，那确实需要在这个阶段做额外工作（读 Program Header、搬移段、清零 BSS）。但我们选了 flat binary，这些工作全部省掉了。

最后是 BIOS 中断的限制。保护模式下 IVT 被 IDT 替换，INT 13h 不再可用。如果要在保护模式下读磁盘，就得像 xv6 那样自己实现 IDE PIO 驱动。对于一个教学 bootloader 来说，在 Real Mode 下用 BIOS 一次性读完所有数据，然后在保护模式和 Long Mode 里不做任何磁盘操作，是最简单的方案。

### 1.3 内存布局整体回顾

到 004B 结束时，低 1MB 内存的完整布局如下：

| 地址 | 用途 | 大小 |
|------|------|------|
| `0x5000` | E820 内存地图（数量 + 条目数组） | 4 + 32*24 B |
| `0x6400` | VESA framebuffer 信息 | 16 B |
| `0x7000` | `BootInfo` 结构体（约定位置，004C 填充） | ~824 B |
| `0x7B00` | DAP 磁盘读取参数包 | 16 B |
| `0x8000` | Stage2 Bootloader 代码 | ~4KB |
| `0x9000~0x19000` | Real Mode 栈 | ~64KB |
| `0x20000~0x88000` | Mini Kernel 镜像（flat binary） | 最大 416KB |
| `0x90000` | Protected/Long Mode 栈 | - |

所有这些地址是"硬编码契约"，bootloader 和内核双方都要遵守。任何地址的修改都必须同步检查所有相关文件。

---

## 二、动手实现

### Step 5: 更新 Mini Kernel 构建配置

**目标**：修改 Mini Kernel 的 CMakeLists.txt，配合 flat binary 加载方案。

**设计思路**：CMakeLists.txt 需要一个 post-build 步骤：用 `objcopy -O binary` 把编译出的 ELF 可执行文件（mini_kernel）转换成 flat binary（mini_kernel.bin）。这一步是必须的——因为 Bootloader 不再解析 ELF 格式，它需要的是纯二进制文件。mini_kernel 库需要包含 `arch/x86_64/boot.S` 作为源文件（注意：boot.S 的完整实现属于 004C 的范围，004B 阶段只需要有一个能编译通过的占位入口），并且 include 路径要包含项目根目录（以便找到 `boot/boot_info.h`）。

Mini Kernel 的链接脚本关键改动是物理地址从 0x200000（2MB）改为 0x20000（128KB）。虚拟地址保持 higher-half 设计：0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000。`objcopy -O binary` 只提取 LMA 对应的内容，所以 flat binary 的第一个字节就是 .text.start 段的 _start 入口。Bootloader 把这个 binary 加载到 0x20000 后，通过页表映射（identity map + higher-half map）就能正确执行。

编译选项中有几个值得关注的：`-mcmodel=large` 允许内核使用任意地址（不只是低 2GB），这对 higher-half 内核是必要的；`-mno-red-zone` 禁用 System V AMD64 ABI 的红区（128 字节的栈下溢保护区），因为内核代码随时可能被中断打断，红区里暂存的数据会被覆盖；`-fno-pie` 和 `-no-pie` 禁用位置无关可执行文件——我们的内核有固定加载地址，不需要 PIC 重定位。

**验证**：构建完成后，确认 `build/kernel/mini/mini_kernel.bin` 文件存在且大小合理（不应该只有几个字节——如果只有 8 字节说明 CMakeLists.txt 没有正确包含 boot.S 和 main.cpp）。用 `xxd build/kernel/mini/mini_kernel.bin | head -5` 查看开头几个字节，应该看不到 0x7F 0x45 0x4C 0x46（ELF magic）——flat binary 没有 ELF header，开头直接就是机器码。另外可以用 `file build/kernel/mini/mini_kernel.bin` 确认文件格式——正确的输出应该是 "data"（因为 flat binary 没有文件头），而不是 "ELF 64-bit LSB executable"。

---

### Step 6: 更新构建脚本

**目标**：修改 `scripts/build_image.sh`，使用 mini_kernel.bin（flat binary）替代 mini_kernel ELF 文件，并加入大小限制检查。

**设计思路**：构建脚本的关键变化有两个。第一，写入磁盘镜像的文件从 mini_kernel ELF 变成了 mini_kernel.bin（flat binary）。ELF 文件有 header、section table 等元数据，实际加载到内存的是"去壳后的纯二进制"，但写入磁盘的应该是 flat binary——因为 Bootloader 不解析 ELF header，它直接把磁盘上的字节搬到内存里。如果磁盘上写的是 ELF 文件，那 0x20000 处放的就是 ELF header（0x7F 'E' 'L' 'F'...），而不是内核代码——CPU 跳过去执行 ELF header 字节的时候，会把这些随机字节当成指令，大概率直接 triple fault。

第二，加入 416KB 的大小限制检查。内核加载区是 0x20000 到 0x88000（416KB），如果 flat binary 超过这个大小就会覆盖 Protected Mode 栈区域（0x90000）。脚本在写入之前先用 stat 获取文件大小，和 MINI_KERNEL_MAX_BYTES（425984 = 416 * 1024）比较，超过就报错退出。错误信息应该包含实际的内存布局约束说明，方便排查。

Mini Kernel 写入磁盘的起始位置仍然是 LBA 16——前面 1 个扇区给 MBR，15 个扇区给 Stage2，LBA 16 开始就是 Mini Kernel 的地盘。写入参数用 `dd ... bs=512 seek=16`，意思是跳到第 16 个扇区开始写入。

**踩坑预警**：如果你发现 debugcon 输出了 0x7F 或者 'E' 'L' 'F' 这样的字节序列而不是内核代码的执行效果，99% 的概率是构建脚本还在用 ELF 文件而不是 .bin 文件写入磁盘。检查 build_image.sh 的第三个参数是不是 mini_kernel.bin 的路径。另一个容易出错的地方是 dd 命令的 seek 参数——必须是 16 而不是其他值，否则磁盘写入位置偏了，Bootloader 从 LBA 16 读出来的就不是内核。

**验证**：完整构建后，用 `xxd build/cinux.img | grep -A2 "00002000:"` 查看 LBA 16（偏移 0x2000 = 8192）处的内容。这里应该是 Mini Kernel 的 flat binary 开头——也就是 boot.S 编译出的机器码，而不是 0x7F 0x45 0x4C 0x46（ELF magic）。如果看到了 ELF magic，说明写入磁盘的是 ELF 文件而不是 flat binary。

---

## 三、构建与运行

在项目根目录执行完整构建：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

构建完成后，确认以下文件存在：

- `build/kernel/mini/mini_kernel.bin`（flat binary，大小应该有几 KB 到几十 KB）
- `build/cinux.img`（磁盘镜像，大小至少 1MB）

然后用 QEMU 运行：

```
make run
```

QEMU 启动后按 `Ctrl+C` 退出，然后检查 debugcon 日志：

```
cat debug.log
```

预期看到字符序列 `OPL`。其中 `O` 表示磁盘读取完成（来自 boot.S 的 load_kernel_from_disk 成功路径，在 Real Mode 输出），`P` 表示 Protected Mode 切换成功（来自 stage2.S 的 pm_entry），`L` 表示 Long Mode 切换成功（来自 stage2.S 的 long_mode_entry）。

如果只看到了 `F`（没有 `P` 和 `L`），说明磁盘读取失败——`F` 是失败路径输出的字符，系统会直接 panic 而不会继续进入 Protected Mode。需要检查 DAP 参数、DL 寄存器是否为 0x80、以及 mini_kernel.bin 是否被正确写入磁盘镜像。

---

## 四、调试技巧

### 检查 flat binary 是否正确写入磁盘

这是 004B 最常见的问题来源。如果你怀疑写入磁盘的不是 flat binary，可以用以下方法确认：

```
xxd build/cinux.img | head -n 300 | grep "00002000"
```

LBA 16 对应的偏移是 0x2000（16 * 512 = 8192）。如果你在这一行看到 `7f45 4c46`（ELF magic），说明写入的是 ELF 文件而不是 flat binary——检查 build_image.sh 的第三个参数和 CMakeLists.txt 的 objcopy post-build 步骤。

### 用 GDB 验证内存中的内核数据

在 GDB 中可以在 `load_kernel_from_disk` 返回后检查 0x20000 处的数据：

```
(gdb) break load_kernel_from_disk
(gdb) continue
(gdb) finish
(gdb) x/16bx 0x20000
```

0x20000 处应该是 Mini Kernel 的 flat binary 开头——也就是 boot.S 编译出的机器码。不应该看到 0x7F 0x45 0x4C 0x46（ELF magic）。如果看到了 ELF magic，和上面的问题一样——写入磁盘的是 ELF 文件。

### 排查栈冲突

如果你发现 Bootloader 在 `load_kernel_from_disk` 返回时崩溃（ret 跳到奇怪地址），很可能是栈冲突。在 GDB 中检查：

```
(gdb) break load_kernel_from_disk
(gdb) info registers ss sp
```

SS 应该是 0x0900，SP 应该是 0xFFFE 附近。物理栈顶 = 0x0900 << 4 + 0xFFFE = 0x19000。内核加载起始地址（MINI_KERNEL_LOAD_PHYS）必须大于 0x19000——如果你发现它被设成了 0x10000，那就是栈冲突了。

如果需要确认磁盘数据是否覆盖了栈区，可以在磁盘读取完成后检查 0x10000~0x19000 范围内的内容——如果这段区域有非零数据（且不是之前写入的），说明内核加载地址太低了。

### 检查内核大小是否超限

如果你发现构建失败，报错信息包含"Mini kernel too large!"，说明 flat binary 超过了 416KB 限制：

```
# 检查 mini_kernel.bin 大小
ls -lh build/kernel/mini/mini_kernel.bin
```

如果超过 416KB，需要：减小小内核代码量，或者调整内存布局（将 protected mode stack 上移）。当前限制来自内存布局的硬约束，不是可以随意调整的。

---

## 本章小结

| 概念 | 要点 |
|------|------|
| BootInfo 结构体 | bootloader-kernel 交接协议，824 字节，packed + static_assert |
| MemoryMapEntry | 24 字节，E820 格式：base(8B) + length(8B) + type(4B) + acpi(4B) |
| Flat Binary | objcopy -O binary 去掉 ELF 元数据，直接加载到目标地址 |
| Flat Binary 代价 | 链接地址必须与加载地址完全匹配，不支持重定位 |
| 分块读取 | BIOS INT 13h 每次最多 127 扇区，循环 7 次读完 832 扇区 |
| 栈冲突 | Real Mode 栈 0x9000~0x19000，加载地址必须 > 0x19000，故用 0x20000 |
| 内存布局 | 栈 0x9000~0x19000，内核 0x20000~0x88000，PM 栈 0x90000 |
| Protected Mode | 过渡阶段，只设段寄存器和栈，不搬运数据 |
| BIOS 寄存器破坏 | INT 13h 破坏 BX/BP，必须 pushw 保存 |
| DAP DS:SI | INT 13h AH=42h 要求 DS:SI（不是 ES:SI）指向 DAP |
| 验证字符序列 | O -> P -> L（004B 预期） |
