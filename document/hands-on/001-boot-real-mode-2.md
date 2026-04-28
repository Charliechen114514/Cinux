# 001_boot_real_mode（下） —— 从文本模式到图形模式

## 导语

上篇我们让 MBR 成功把 Stage2 加载到了 0x8000 并跳转过去，QEMU 屏幕上能看到 `Cinux Booting...` 和 `Stage2 OK`。但 Stage2 目前什么也没干——这篇我们要让它做三件关键的事情：开启 A20 地址线突破 1MB 限制，通过 VESA BIOS 设置 1024×768×32 图形模式，把帧缓冲信息保存好留给后续的内核使用。

完成后，QEMU 会从文本模式切换到图形模式（屏幕变黑——别慌，这是正常的），GDB 能验证帧缓冲物理地址和分辨率信息已经正确保存到了 0x6400。

前置要求：完成上篇，MBR 能成功加载并跳转 Stage2。

## 概念精讲

### A20 地址线——从历史遗留问题说起

在 8086 时代，CPU 只有 20 根地址线（A0–A19），能寻址 1MB。当程序试图访问超过 1MB 的地址时（比如 `0xFFFF:0x0010` = 0x100000），地址会回绕到 0（变成 0x00000）。有些早期的程序依赖了这个回绕行为。到了 286 时代，CPU 有了更多地址线，为了兼容这些老程序，Intel 决定默认关闭第 21 根地址线（A20），让回绕行为保持不变。

OSDev Wiki 的 A20 Line 页面详细记录了这段历史和四种开启方式。对我们来说最重要的是：进入保护模式之前必须开启 A20，否则访问超过 1MB 的内存会出问题。Cinux 使用最安全的方式——BIOS INT 0x15 AX=0x2401。如果 BIOS 不支持，还可以尝试 Fast A20（读写端口 0x92）或键盘控制器方式（8042 芯片），不过 QEMU 里 BIOS 方式总是有效的。

```
A20 关闭 vs 开启：

  地址线:  A19 A18 ... A1  A0     A20
  关闭时:  正常工作，但 A20 被强制为 0
           0x100000 → 0x000000 (回绕!)
           0x10FFEF → 0x00FFEF

  开启后:  A20 正常参与地址计算
           0x100000 → 0x100000 (终于能用了)
           0x10FFEF → 0x10FFEF (High Memory Area)
```

### VESA/VBE——用 BIOS 设置图形模式

VESA BIOS Extensions（VBE）是显卡厂商和 VESA 组织定义的一套标准接口，让 bootloader 和操作系统可以通过 BIOS 调用来查询和设置高分辨率图形模式。OSDev Wiki 的 VBE 页面告诉我们，VBE 2.0+ 支持线性帧缓冲（Linear Framebuffer）：显存被映射到一段连续的物理地址，直接往那个地址写数据就能在屏幕上显示像素。

Cinux 使用三步流程：

1. **获取控制器信息**（INT 0x10 AX=0x4F00）：确认 VBE 2.0+ 可用，在 0x6000 放一个 512 字节的 VbeInfoBlock，调用前要写入 `"VBE2"` 签名告诉 BIOS 我们要 2.0 版本的信息。
2. **获取模式信息**（INT 0x10 AX=0x4F01）：查询模式 0x118（1024×768×32bpp）的详细信息，在 0x6200 放一个 256 字节的 ModeInfoBlock，从中提取帧缓冲物理地址、pitch、分辨率等字段。
3. **设置视频模式**（INT 0x10 AX=0x4F02）：传入 `0x4118`（模式号 0x118 加上 bit14=1 启用线性帧缓冲），切换完成后屏幕会变黑（从文本模式切换到图形模式）。

```
VBE 调用流程：

  Step 1: Controller Info (0x4F00)
    ES:DI → 0x6000 (VbeInfoBlock, 512B)
    签名: "VBE2"
    验证: AX=0x004F

  Step 2: Mode Info (0x4F01)
    CX = 0x0118 (模式号)
    ES:DI → 0x6200 (ModeInfoBlock, 256B)
    提取: PhysBasePtr[+0x28], Pitch[+0x10], Width[+0x12], Height[+0x14]

  Step 3: Set Mode (0x4F02)
    BX = 0x4118 (0x118 | 0x4000 = 启用线性帧缓冲)
    屏幕变黑 ← 正常现象！

ModeInfoBlock 关键字段：
  +0x10  2B  BytesPerScanLine (pitch)
  +0x12  2B  XResolution
  +0x14  2B  YResolution
  +0x19  1B  BitsPerPixel
  +0x28  4B  PhysBasePtr (帧缓冲物理地址)
```

### 线性帧缓冲——像素的直接映射

设置完 VESA 模式后，显存被映射到一个物理地址（QEMU 里通常是 0xFD000000）。这就是线性帧缓冲——一个连续的内存区域，每个像素按顺序排列，写入颜色值就能在屏幕对应位置显示。

在 32bpp 模式下每个像素占 4 字节（ARGB 格式），一行 1024 个像素就是 4096 字节（这个值叫 pitch 或 BytesPerScanLine，QEMU 的默认值）。坐标 (x, y) 对应的地址 = PhysBasePtr + y × pitch + x × 4。后续内核的图形驱动会大量使用这个公式，所以我们需要在 bootloader 阶段把这几个参数保存好。

### 共享函数库——common/serial.S

Stage2 链接了 common/serial.S，里面包含所有引导阶段共用的函数。这个文件里有 print_string（字符串输出）、panic（打印错误信息并停机）、enable_a20（A20 开启）、vesa_get_controller_info、vesa_get_mode_info、vesa_set_mode、vesa_save_framebuffer_info 这七个函数。它们都是 `.code16gcc` 格式（兼容 --32 编译的 16 位代码），使用 `.global` 导出供 stage2.S 调用。

### 内存布局总览

```
0x0000 – 0x4FFF   IVT + BIOS 数据区（别碰）
0x5000 – 0x5FFF   空闲
0x6000             VbeInfoBlock (512B) — VESA 控制器信息
0x6200             ModeInfoBlock (256B) — VESA 模式信息
0x6400             FB Info (16B) — 帧缓冲信息
                    +0  8B  物理地址
                    +8  4B  pitch (每行字节数)
                    +C  2B  宽度 (像素)
                    +E  2B  高度 (像素)
0x7000             MBR 栈底 (向下增长)
0x7B00             DAP 结构 (16B)
0x7C00 – 0x7DFF    MBR (512B)
0x8000 – 0x9DFF    Stage2 代码 (最多 7.5KB)
0x9000:0xFFFE      Stage2 栈 (SS=0x0900, SP=0xFFFE)
```

## 动手实现

### Step 5: 开启 A20 地址线

**目标**：通过 BIOS INT 0x15 AX=0x2401 开启 A20 地址线。

**设计思路**：这是最简单也最安全的 A20 开启方式。调用前不需要特别的参数准备，调用后检查进位标志 CF——如果 CF=0 说明成功，CF=1 说明失败。失败的情况在 QEMU 中基本不会出现（QEMU 的 BIOS 完全支持这个调用），但在真机上如果遇到失败，可以回退到 Fast A20（端口 0x92 的 bit 1 置 1）。

**实现约束**：
- 函数 `enable_a20` 在 common/serial.S 中定义
- 调用 INT 0x15 时 AX=0x2401
- 成功时返回，失败时调用 panic 打印错误信息并停机
- 调用前后需要保存和恢复寄存器（BIOS 会修改部分寄存器）
- 错误信息字符串定义在 common/serial.S 的 `.data` 段

**踩坑预警**：有些教程会建议直接操作端口 0x92 来开启 A20（所谓的 Fast A20），但这种方式在某些硬件上会同时触发系统重启（端口 0x92 的 bit 0 是 reset line）。BIOS 方式虽然多一次中断调用的开销，但安全得多。另外，A20 开启函数应该在打印 `"Stage2 OK"` 之后、VESA 操作之前调用。

**验证**：在 Stage2 中调用 `enable_a20` 后打印一条确认消息（比如 `"A20 OK"`）。QEMU 应该不会停机（如果 A20 开启失败会进入 panic 死循环）。

### Step 6: 获取 VESA 控制器信息

**目标**：调用 INT 0x10 AX=0x4F00 获取 VBE 控制器信息，验证 VBE 2.0+ 可用。

**设计思路**：这是 VESA 三步流程的第一步。我们需要在内存中准备一个 512 字节的缓冲区（放在 0x6000），调用前写入 `"VBE2"` 签名（4 字节，表示请求 VBE 2.0+ 格式的信息），然后设置 ES:DI 指向这个缓冲区并调用。返回后检查 AL=0x4F（功能支持）且 AH=0（调用成功）。VbeInfoBlock 里包含了 VBE 版本号、OEM 信息、可用模式列表等，但当前我们只需要确认调用成功就够了。

**实现约束**：
- 函数 `vesa_get_controller_info` 在 common/serial.S 中定义
- 缓冲区地址：0x6000（ES 段 = 0x6000 >> 4 = 0x600，DI = 0）
- 调用前在 ES:DI 处写入 4 字节签名：`'V','B','E','2'`（小端序 0x4256, 0x3245）
- INT 0x10 AX=0x4F00
- 检查返回值：AL=0x4F 且 AH=0 表示成功
- 失败时调用 panic 并打印错误信息

**踩坑预警**：ES 段的计算容易搞错。0x6000 的段地址是 0x6000 / 16 = 0x600，不是 0x6000。如果你直接把 0x6000 写进 ES，实际指向的地址会是 0x60000——完全不对。签名也是：必须在小端序下写入 0x4256 和 0x3245（对应 ASCII "VB" 和 "E2"），不是直接写 "VBE2" 的 ASCII 码。还有，调用前 ES:DI 必须正确设置，否则 BIOS 写入的位置会错。

**验证**：调用后如果没进 panic，说明控制器信息获取成功。可以在调用前后打印调试信息确认。

### Step 7: 获取 VESA 模式信息

**目标**：调用 INT 0x10 AX=0x4F01 获取模式 0x118 的详细信息。

**设计思路**：有了控制器信息确认 VBE 可用后，我们查询目标模式 0x118（1024×768×32bpp）的具体参数。ModeInfoBlock 是 256 字节，放在 0x6200。这个结构包含了我们后续需要的所有信息：帧缓冲物理地址（偏移 +0x28，4 字节）、每行字节数 pitch（偏移 +0x10，2 字节）、水平分辨率（偏移 +0x12，2 字节）、垂直分辨率（偏移 +0x14，2 字节）、色深（偏移 +0x19，1 字节）。

**实现约束**：
- 函数 `vesa_get_mode_info` 在 common/serial.S 中定义
- 缓冲区地址：0x6200（ES 段 = 0x6200 >> 4 = 0x620，DI = 0）
- INT 0x10 AX=0x4F01，CX=0x0118（模式号）
- 检查返回值：AL=0x4F 且 AH=0
- 失败时 panic
- 此时仍在文本模式，可以打印 `"Mode info OK"` 确认

**踩坑预警**：模式号是 0x118 不是 0x4118——后者是设置模式时才加的（bit14 表示启用线性帧缓冲）。查询模式信息时用纯模式号。QEMU 默认支持 0x118 模式，但在真机上可能需要枚举可用模式找一个支持的。如果这一步失败，很可能是 QEMU 版本太旧不支持这个模式。

**验证**：在 GDB 中检查 0x6200 开始的 ModeInfoBlock 内容。特别注意偏移 +0x28 的 PhysBasePtr，在 QEMU 中应该是 0xFD000000。用 `x/4bx 0x6228` 查看。

### Step 8: 设置视频模式并保存帧缓冲信息

**目标**：调用 INT 0x10 AX=0x4F02 设置图形模式，然后把关键参数保存到 0x6400。

**设计思路**：先设置模式再保存信息——看起来有点反直觉，但实际上 ModeInfoBlock 在 Step 7 就已经填好了，设置模式只是让显卡实际切换到这个模式。BX 寄存器传入 `0x4118` = 模式号 0x118 + bit14（0x4000，启用线性帧缓冲）。调用成功后屏幕会变黑——因为图形模式下没有文本光标和字符输出，所有之前打印的内容都消失了。

设置模式后，我们从 ModeInfoBlock（0x6200）提取四个关键字段写入 FB Info 结构（0x6400）：物理地址（8 字节，从 +0x28 取 4 字节零扩展到 8 字节）、pitch（4 字节，从 +0x10 取 2 字节）、宽度（2 字节，从 +0x12）、高度（2 字节，从 +0x14）。总共 16 字节。这些信息后续内核初始化图形驱动时要用。

**实现约束**：
- 函数 `vesa_set_mode`：INT 0x10 AX=0x4F02，BX=0x4118，检查 AL=0x4F 且 AH=0
- 函数 `vesa_save_framebuffer_info`：从 0x6200 读 ModeInfoBlock 字段，写入 0x6400 的 FB Info
  - ES 段 = 0x620（ModeInfoBlock），GS 段 = 0x640（FB Info）
  - 需要使用段超越前缀（segment override prefix）访问不同段的数据
  - 写入布局：+0 物理地址(8B)、+8 pitch(4B)、+C width(2B)、+E height(2B)
- 保存帧缓冲信息时必须 push/pop ES 和 GS（因为我们要临时修改它们）

**踩坑预警**：设置模式后文本输出就废了——INT 0x10 AH=0x0E 的 teletype 输出在图形模式下要么不工作，要么效果很奇怪。所以所有调试信息的打印都必须在设置模式之前完成。如果设置模式之后需要调试，唯一的办法是用 GDB 直接查看内存和寄存器。保存帧缓冲信息时有个隐蔽的坑：在 16 位实模式下，跨段访问必须用段超越前缀，否则编译器默认用 DS 段——但你需要的两个数据源分别在 ES 和 GS 段。

**验证**：这一步没有屏幕输出可以看，只能用 GDB 验证。在 Stage2 的 halt 循环处设断点，检查 0x6400 的内容：

```
(gdb) x/2gx 0x6400
0x6400: 0x00000000fd000000  0x0000100000000400
```

解析：PhysBasePtr = 0xFD000000，pitch 应该是 0x1000（4096），宽度 0x0400（1024），高度 0x0300（768）。如果你的值类似这样，说明一切正常。

如果想进一步验证帧缓冲本身可用，可以在 GDB 里直接写像素：

```
(gdb) set {int}0xfd000000 = 0x00FF0000
```

这会在屏幕左上角写一个红色像素——不过单个像素很难看清，可以写一个色块：

```
(gdb) set $y = 0
(gdb) while $y < 100
 > set $x = 0
 > while $x < 100
  > set {int}(0xfd000000 + ($y * 4096 + $x * 4)) = 0x00FF0000
  > set $x = $x + 1
 > end
 > set $y = $y + 1
(gdb) end
```

左上角出现一个红色方块，恭喜——你的图形模式设置成功。

### Step 9: 更新构建系统

**目标**：修改 CMakeLists.txt 和 build_image.sh 支持 MBR + Stage2 双目标构建。

**设计思路**：tag 000 的 CMakeLists 只有一个 mbr 目标，现在需要添加 stage2。两个目标都使用相同的编译选项 `-Wa,--32`，但链接脚本不同：MBR 的起始地址是 0x7C00（实际运行地址），Stage2 的起始地址是 0（因为段寄存器会负责偏移）。两者都链接为 elf32-i386，然后 objcopy 转为纯二进制。build_image.sh 用 dd 把 mbr.bin 写到扇区 0，stage2.bin 写到扇区 1 开始的位置。

**实现约束**：
- 添加一个 `boot_common` OBJECT 库目标（包含 common/serial.S）
- stage2 链接 stage2.S + boot_common 目标文件
- mbr 只链接 mbr.S（不链接 boot_common——512 字节限制）
- 两个链接脚本都用 `file(WRITE ...)` 在构建目录生成
- objcopy 转换后的文件名：mbr.bin、stage2.bin
- build_image.sh 接收三个参数：mbr bin 路径、stage2 bin 路径、输出镜像路径
- cmake/qemu.cmake 中 DEPENDS 要同时依赖 mbr 和 stage2

**踩坑预警**：最常见的错误是忘记在 cmake/qemu.cmake 的 DEPENDS 列表里加上 stage2——结果修改了 stage2.S 重新构建时镜像不会更新。另一个是链接脚本的 OUTPUT_FORMAT 必须是 `elf32-i386` 不是 `elf64-x86-64`——虽然我们跑的是 x86_64 机器，但 16 位实模式代码必须用 32 位 ELF 格式。

**验证**：修改任意一个 .S 文件后重新 `cmake --build build`，确认两个 bin 文件和镜像都被更新。用 `ls -la build/boot/mbr.bin build/boot/stage2.bin` 检查文件大小——mbr.bin 应该恰好 512 字节，stage2.bin 应该在 1-2KB 左右。

## 构建与运行

完整构建流程不变：

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
qemu-system-x86_64 -drive format=raw,file=build/cinux.img
```

如果需要调试图形模式部分，推荐用 GDB：

```
qemu-system-x86_64 -S -gdb tcp::1234 -drive format=raw,file=build/cinux.img &
gdb
  (gdb) target remote :1234
  (gdb) set architecture i8086
  (gdb) break *0x7c00
  (gdb) continue
```

Stage2 在 0x8000 开始执行，GDB 中需要计算实际地址（CS × 16 + IP）。

## 调试技巧

### VESA 调试的核心思路

VESA 初始化过程分为"文本模式阶段"和"图形模式阶段"。文本模式下可以用 INT 0x10 打印调试信息，设置模式后只能靠 GDB。所以我们的策略是：在设置模式之前尽可能多打印状态信息，设置模式之后用 GDB 直接检查内存。

### GDB 验证帧缓冲信息

```
(gdb) x/4xw 0x6400     # 查看低 16 字节
```

预期值（QEMU）：
- +0x00: 0xFD000000（帧缓冲物理地址）
- +0x08: 0x00001000（pitch = 4096）
- +0x0C: 0x0400（宽 = 1024）
- +0x0E: 0x0300（高 = 768）

### 常见故障排查

| 现象 | 可能原因 | 排查方法 |
|------|----------|----------|
| A20 开启后 panic | QEMU 版本过旧 | 检查 INT 0x15 返回值 |
| VESA 控制器信息失败 | ES:DI 或签名设错 | GDB 检查 0x6000 处签名 |
| 模式 0x118 不可用 | QEMU 不支持此模式 | 尝试 0x117 (1024×768×16) |
| 设置模式后三屏重启 | 模式号错误或 LFB 未启用 | 检查 BX 是否包含 0x4000 |
| FB Info 全是零 | 段超越前缀缺失 | GDB 单步检查 ES/GS 读取 |

## 本章小结

| 概念 | 关键点 |
|------|--------|
| A20 地址线 | INT 0x15 AX=0x2401，CF=0 成功 |
| VBE 控制器信息 | INT 0x10 AX=0x4F00，ES:DI→0x6000，签名 "VBE2" |
| VBE 模式信息 | INT 0x10 AX=0x4F01，CX=0x118，ES:DI→0x6200 |
| VBE 设置模式 | INT 0x10 AX=0x4F02，BX=0x4118 (模式+LFB) |
| ModeInfoBlock 字段 | PhysBasePtr[+0x28], Pitch[+0x10], W[+0x12], H[+0x14] |
| FB Info (0x6400) | addr(8B) + pitch(4B) + w(2B) + h(2B) = 16B |
| 线性帧缓冲 | 像素地址 = PhysBasePtr + y × pitch + x × 4 |
| common/serial.S | 共享函数库，stage2 链接，MBR 不链接 |
