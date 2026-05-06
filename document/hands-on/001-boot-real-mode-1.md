# 001_boot_real_mode（上） —— 让 BIOS 把接力棒交到我们手里

## 导语

上一章我们把工具链搭好了，CMake 能编译出一个什么也不干的 MBR stub，QEMU 能跑起来然后直接停机。现在我们要让这个 stub 变成真正的引导程序：BIOS 把磁盘第一个扇区读到 0x7C00，我们的代码从那里接手，在屏幕上打印出第一行字，然后把第二阶段引导程序（Stage2）从磁盘读进来。

完成本篇后，QEMU 屏幕上会出现 `Cinux Booting...` 的字样，然后 Stage2 被加载到内存并跳转过去。下一篇我们会让 Stage2 干更多事情——开启 A20、初始化 VESA 图形模式——但这一篇的目标是先把 MBR 这 512 字节写对。

前置要求：完成 tag 000 的环境搭建，能成功编译和运行 MBR stub。

## 概念精讲

### 实模式——CPU 上电后的默认状态

按一下电源键，现代 x86 CPU 上电后从物理地址 0xFFFFFFF0 取第一条指令（Intel SDM Vol.3A §10.1.1），该地址映射到 BIOS ROM 尾部。此刻 CPU 处于**实模式**（Real-Address Mode）：16 位寄存器，没有内存保护，没有虚拟地址，段寄存器直接参与物理地址的计算。

实模式的地址计算公式是 `物理地址 = 段 × 16 + 偏移`，其中段和偏移都是 16 位值。这个公式的效果是：20 根地址线，寻址空间从 0x00000 到 0xFFFFF，刚好超过 1MB。比如 `0x07C0:0x0000` 和 `0x0000:0x7C00` 指向同一个物理地址 0x7C00——这就是 BIOS 加载 MBR 的位置。

```
实模式寻址示意：

  段寄存器 (16-bit)    偏移 (16-bit)
       ↓                    ↓
    0x07C0     ×  16  +  0x0000  =  0x7C00
    0x0000     ×  16  +  0x7C00  =  0x7C00   ← 同一个地址！

  最大寻址：
    0xFFFF     ×  16  +  0xFFFF  =  0x10FFEF  (需要 A20 开启)
    A20 关闭时 → 0x00FFEF  (回绕到 1MB 以内)
```

### MBR——512 字节的荒岛求生

MBR 是磁盘的第一个扇区，固定 512 字节，最后两个字节必须是 `0x55 0xAA`（小端序的 0xAA55）。BIOS 做完 POST 自检后，会把启动盘的第一个扇区读到 0x7C00，检查最后两字节是否为 0xAA55，然后跳过去执行。

OSDev Wiki 的 MBR 页面告诉我们一个关键事实：BIOS 跳转时 CS:IP 可能是 `0x0000:0x7C00` 也可能是 `0x07C0:0x0000`，取决于 BIOS 实现。其他所有寄存器的值都是**未定义的**。这意味着我们做的第一件事必须是规范化 CS——用一条远跳转 `ljmp $0, $real_start` 强制设置 CS=0。

```
BIOS → MBR 启动流程：

  Power On
    ↓
  POST (加电自检)
    ↓
  BIOS 读磁盘扇区 0 → 加载到 0x7C00
    ↓
  检查偏移 510-511 是否为 0x55 0xAA
    ↓
  跳转到 CS:IP 执行
    ↓
  [你的代码] ← 接力棒在这里交接

MBR 512B 布局：
  ┌──────────────────────────┐ 0x000
  │ 引导代码 (~446 bytes)     │
  ├──────────────────────────┤ 0x1BE
  │ 分区表 (4 × 16 bytes)     │
  ├──────────────────────────┤ 0x1FE
  │ 签名 0x55 0xAA            │
  └──────────────────────────┘ 0x200
```

### DAP——告诉 BIOS 从磁盘哪里读、读到内存哪里

传统 CHS（柱面-磁头-扇区）寻址方式只能访问 8GB 以下的磁盘，而且参数特别麻烦。LBA（Logical Block Addressing）用线性编号定位扇区，简单得多。OSDev Wiki 的 INT 13h 页面告诉我们，BIOS 提供了扩展读取接口 `INT 0x13 AH=0x42`，配合一个叫 DAP（Disk Address Packet）的数据结构。

DAP 是 16 字节的结构，描述了要读取的扇区数量、目标内存地址（段:偏移格式）和起始 LBA 号。我们把它构造在 0x7B00（紧贴 MBR 下方），告诉 BIOS 从 LBA 1 开始读 15 个扇区（Stage2 的内容）到 0x0000:0x8000。

```
DAP 结构布局（16 字节）：

  偏移  大小    字段
  0x00  1 byte  结构大小 = 0x10 (固定)
  0x01  1 byte  保留 = 0
  0x02  2 bytes 要读取的扇区数
  0x04  2 bytes 目标缓冲区偏移
  0x06  2 bytes 目标缓冲区段
  0x08  4 bytes 起始 LBA 低 32 位
  0x0C  4 bytes 起始 LBA 高 32 位
```

## 动手实现

### Step 1: 创建 MBR 入口骨架

**目标**：建立一个能被 BIOS 正确识别并开始执行的 MBR 文件。

**设计思路**：BIOS 跳转后 CS 不确定，所以第一条指令就是远跳转 `ljmp $0, $real_start` 来规范化 CS=0。跳转后立即关中断（`cli`），因为初始化段寄存器和栈的过程中中断处理程序可能用到还没设置好的段地址。然后把 DS、ES、FS、GS、SS 全部设成和 CS 一样（都是 0），保证所有内存访问都用统一的地址模型。方向标志用 `cld` 清零（确保 lodsb 等字符串操作是正向递增的）。栈指针设到 0x7000（在 MBR 代码下方，向下增长，不会覆盖 MBR 本身）。最后开中断 `sti`，因为后续要调用 BIOS 中断服务。

**实现约束**：
- 文件使用 `.code16` 指令生成 16 位代码
- 用 `.set` 定义常量：`STACK_BASE_ADDR = 0x7000`
- 入口标签 `_start` 必须 `.global` 导出给链接器
- 数据段放在代码段后面，用 `.fill` 填充到 510 字节，最后用 `.word 0xAA55` 写入签名

**踩坑预警**：千万别忘了 `ljmp` 那条跳转。如果跳过它直接往下写，某些 BIOS 上 CS 不为 0，后面所有段:偏移地址都会算错，然后你会发现代码偶尔能跑偶尔不能跑，血压拉满。另外，栈不能放在 0x7B00 或更靠近 MBR 的地方——BIOS 自己也会用栈，加上你的 push 操作，很容易踩到 MBR 代码区域导致莫名崩溃。

**验证**：这一步先写个最简版本——入口规范化段寄存器后直接 `cli; hlt`，加上填充和签名。编译运行后 QEMU 应该启动然后立即停机（黑屏，CPU halted），说明 BIOS 成功识别并执行了你的 MBR。

构建命令参考：

```
cmake --build build
qemu-system-x86_64 -drive format=raw,file=build/cinux.img
```

### Step 2: 实现屏幕字符输出

**目标**：在 MBR 内实现一个打印 null 终止字符串的函数，能在屏幕上显示启动消息。

**设计思路**：BIOS 提供了 `INT 0x10 AH=0x0E`（Teletype Output）这个中断服务，它会把 AL 中的字符打印到屏幕当前光标位置，自动处理换行回车。我们的字符串输出函数就是循环调用这个中断：用 `lodsb` 从 DS:SI 读一个字节到 AL（SI 自动加 1），检查是否为 0（字符串结束符），不是就调 INT 0x10 打印，是就返回。Intel SDM Vol.3A §21.1.1 提到实模式下 DS:SI 就是数据访问的标准方式，而 `lodsb` 恰好依赖这个组合。

**实现约束**：
- 函数输入：SI 寄存器指向 null 终止字符串
- 函数修改的寄存器：AX、BX、SI（调用前需要 push 保存）
- 调用 BIOS 时 AH=0x0E，BH=页号（0），BL=颜色属性（文本模式下可忽略）
- 方向标志必须清除（`cld`），否则 lodsb 会反向递减 SI
- 这个函数定义在 MBR 文件内部（不放在 common.S 里），因为 MBR 有 512 字节限制

**踩坑预警**：段寄存器不匹配是这里最常见的坑。如果 CS 不等于 DS，字符串地址会算错——你用 CS:offset 定义的字符串，但 lodsb 用 DS:SI 去读。Step 1 里我们已经让所有段寄存器统一为 0 了，所以只要不在中途修改 DS 就没问题。另一个坑是忘记 `cld`——如果方向标志被 BIOS 设成了 1，lodsb 会反向递减 SI，你会读到字符串前面的垃圾数据。还有一点：BIOS INT 调用会破坏某些寄存器的值（DS、ES、FLAGS 等都可能被改），所以如果你的函数还需要后续使用某些寄存器，调用前后必须保存恢复。

**验证**：在 Step 1 的基础上，定义一个字符串 `msg_booting` 内容为 `"Cinux Booting...\r\n"`，在段寄存器初始化后、hlt 之前调用打印函数。QEMU 屏幕上应该能看到这行字出现。

### Step 3: 从磁盘加载 Stage2

**目标**：使用 BIOS INT 0x13 AH=0x42 扩展读取，把 Stage2 从磁盘读到内存 0x8000。

**设计思路**：MBR 只有 512 字节，做不了太多事情。我们的策略是 MBR 只负责最小化的初始化和磁盘读取，把复杂工作交给 Stage2。磁盘上 Stage2 紧跟在 MBR 之后（LBA=1），我们读取 15 个扇区（7.5KB）到 0x8000。读取使用 DAP 结构描述参数：目标地址 0x0000:0x8000，起始 LBA=1，扇区数=15。注意 DL 寄存器必须保存 BIOS 传入的启动盘号——MBR 入口时 BIOS 会把盘号放在 DL 里，我们把它存到变量 `boot_drive` 中，读取磁盘时再恢复。

**实现约束**：
- DAP 结构构造在 0x7B00（用 `.set DAP_STORE_ADDR, 0x7B00`）
- DAP 各字段按偏移填写：大小=0x10，扇区数=15，偏移=0x8000，段=0，LBA 低 32 位=1，高 32 位=0
- INT 0x13 调用时 AH=0x42（扩展读取），DL=启动盘号，DS:SI 指向 DAP
- 进位标志 CF=1 表示读取失败，需要打印错误信息并停机
- 读取成功后用远跳转 `ljmp` 跳到 Stage2：段=0x0800（即 0x8000 >> 4），偏移=0

**踩坑预警**：DAP 的偏移 0x01 那个保留字节必须填 0（有的 BIOS 不检查，有的会因此失败）。另一个大坑是把 MBR 和 common.S 链接在一起导致 .text 段超过 512 字节——BIOS 只读第一个扇区，后面的代码根本没被加载到内存，跳过去就是执行垃圾数据。Cinux 的解决方案是 MBR 完全自包含，不链接任何外部文件。还有一个细节：远跳转到 Stage2 时段地址是 0x0800 而不是 0，因为 Stage2 的代码从 0x8000 开始执行。

**验证**：在 MBR 加载 Stage2 后打印一条启动消息。QEMU 屏幕上应该出现 `Cinux Booting...`，然后 Stage2 被跳转到（此刻 Stage2 还没写，跳转后会执行随机内容或重启——这是正常的）。

### Step 4: 创建 Stage2 最小入口

**目标**：创建 Stage2 文件，被 MBR 跳转后能打印确认消息。

**设计思路**：Stage2 被 MBR 的 `ljmp $0x0800, $0` 跳转过来，此时 CS=0x0800。和 MBR 一样，第一件事是重新规范化段寄存器——把 DS、ES 等都设成和 CS 一样。栈需要单独处理：我们用 SS=0x0900、SP=0xFFFE，这样栈空间在 0x9000:0xFFFE 即物理地址 0x9FFFE 附近，远离 Stage2 代码区域。然后调用 common.S 中的 `print_string` 打印 `"Stage2 OK\r\n"`。

**实现约束**：
- Stage2 文件使用 `.code16` 指令
- 入口标签 `_start` 同样需要 `.global` 导出
- 链接脚本中 `. = 0`（起始地址为 0，因为段寄存器会负责地址偏移）
- Stage2 链接了 common/serial.S 的目标文件（包含 print_string 等函数）
- 用 `.extern` 声明需要使用的外部函数

**踩坑预警**：Stage2 链接脚本的起始地址必须设为 0，不能设为 0x8000。原因：CS=0x0800 已经提供了段偏移（0x0800 × 16 = 0x8000），如果链接地址再写 0x8000 就成了双重偏移。这是实模式引导中最隐蔽的地址模型错误——你能执行代码（因为 CS:IP 计算正确），但一旦访问数据段就会崩（因为 DS:offset 里的 offset 也带了 0x8000 的偏移）。

**验证**：构建并运行，QEMU 屏幕上应该依次显示 `Cinux Booting...`、`Stage2 OK`，然后停机。

## 构建与运行

完整的构建流程：

构建使用 CMake，CMakeLists.txt 中定义了两个独立的可执行目标——`mbr` 和 `stage2`。mbr 只包含 mbr.S，stage2 包含 stage2.S 和 common/serial.S 的目标文件。两个目标都使用 `-Wa,--32` 编译为 32 位目标文件（内含 16 位代码段），链接为 elf32-i386 格式，然后用 `objcopy -O binary` 转为纯二进制。

`build_image.sh` 脚本负责组装磁盘镜像：用 `dd` 创建一个空文件，然后把 mbr.bin 写到扇区 0（偏移 0），stage2.bin 写到扇区 1 开始的位置（偏移 512 字节）。

运行命令：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
qemu-system-x86_64 -drive format=raw,file=build/cinux.img
```

QEMU 参数说明：`-drive format=raw,file=...` 指定使用原始格式的磁盘镜像。后续调试时可以加 `-serial stdio` 把串口输出重定向到终端，或者加 `-S -gdb tcp::1234` 启动时暂停并开放 GDB 调试端口。

## 调试技巧

### QEMU Monitor 检查 MBR 加载

在 QEMU 运行时按 `Ctrl+A, C` 进入 monitor 模式，可以用 `xp /512bx 0x7c00` 查看 MBR 是否被正确加载到内存。最后两个字节应该是 `55 aa`。

### GDB 单步调试

```
qemu-system-x86_64 -S -gdb tcp::1234 -drive format=raw,file=build/cinux.img &
gdb
  (gdb) target remote :1234
  (gdb) set architecture i8086
  (gdb) break *0x7c00
  (gdb) continue
```

进入 MBR 后可以单步执行，观察段寄存器的变化。注意实模式下 GDB 显示的地址需要手动计算 `CS × 16 + IP`。

### 常见故障排查

| 现象 | 可能原因 | 排查方法 |
|------|----------|----------|
| QEMU 反复重启 | MBR 签名缺失或错误 | 检查 0x7DFE-0x7DFF 是否为 55 AA |
| 字符串不显示 | CS≠DS 或方向标志未清 | GDB 检查 DS 和 EFLAGS.DF |
| Stage2 跳转后崩溃 | 磁盘读取失败或地址算错 | GDB 在 0x8000 设断点检查是否到达 |
| `int $0x13` 后 CF=1 | DAP 字段填错或盘号不对 | 检查 DL 和 DAP 各字段 |

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 实模式寻址 | 物理地址 = 段 × 16 + 偏移，20 位地址空间 |
| MBR | 512B，0x7C00 加载，0xAA55 签名，CS 不确定需 ljmp 规范化 |
| BIOS INT 0x10 AH=0x0E | Teletype 字符输出，AL=字符 |
| DAP | 16B 结构，描述 LBA 读盘参数 |
| INT 0x13 AH=0x42 | 扩展磁盘读取，DS:SI 指向 DAP |
| MBR/Stage2 地址模型 | MBR 链接地址 0x7C00 + DS=0；Stage2 链接地址 0 + DS=CS |
| 内存布局 | 0x7000 栈、0x7B00 DAP、0x7C00 MBR、0x8000 Stage2 |

## 参考资料

- Intel SDM: Vol.3A §10.1.1 — Processor State After Reset (CS=0xF000, EIP=0xFFF0, 复位后初始状态)
- Intel SDM: Vol.3A §21.1.1 — Real-Address Mode (段:偏移寻址公式)
- OSDev Wiki: [MBR (x86)](https://wiki.osdev.org/MBR_(x86)) — MBR 格式、BIOS 初始环境、签名规范
- OSDev Wiki: [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) — DAP 结构、扩展读盘 AH=0x42
- OSDev Wiki: [A20 Line](https://wiki.osdev.org/A20_Line) — A20 地址线历史和开启方式
