# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☑` 打完后打 tag，触发 prompts/ 工作流

---

## Phase 1 · Bootloader

### `001_boot_real_mode`
**效果**：QEMU 图形窗口左上角依次出现 `Cinux Booting...` → `Stage2 OK`（均通过 BIOS INT 0x10 屏幕输出）

> **验证手段**：全程 BIOS `INT $0x10 AH=0x0E` 屏幕字符输出，不碰串口

- ☑ `boot/mbr.S`：`.code16`，`ljmp $0,$real_start` 规范化 CS，清零 `%ds %es %ss %sp`，`movb %dl, boot_drive` 保存启动盘号
- ☑ 实现 `print_string`：`lodsb` + BIOS `INT $0x10 AH=0x0E` 循环输出，以 `\0` 结尾
- ☑ 打印 `msg: .asciz "Cinux Booting..."`
- ☑ 末尾加 DAP 结构（`dap`）：size=0x10，sectors=4，dest=`0x0000:0x8000`，LBA=1
- ☑ 调 `INT $0x13 AH=0x42`（扩展磁盘读）将 stage2 载入 `0x8000`
- ☑ `ljmp $0, $0x8000` 跳转 stage2
- ☑ `boot/stage2.S`：入口用 BIOS `INT $0x10` 打印 `Stage2 OK`，后续步骤在此文件继续
- ☑ `scripts/build_image.sh` 更新：sector 0 写 MBR，sector 1+ 写 stage2.bin
- ☑ 开启 A20：`INT $0x15 AX=0x2401`
- ☑ VESA 控制器信息：`INT $0x10 AX=0x4F00`，ES:DI 指向 512 字节 `VbeInfoBlock` 缓冲（放 `0x6000`），验证返回 `AX=0x004F`
- ☑ 枚举目标模式 `0x118`（1024×768×32bpp linear framebuffer）：`INT $0x10 AX=0x4F01 CX=0x118`，ES:DI 指向 256 字节 `ModeInfoBlock`（放 `0x6200`），取 `PhysBasePtr`（偏移 `0x28`，4字节）、`BytesPerScanLine`（偏移 `0x10`）、`XResolution/YResolution`（偏移 `0x12/0x14`）、`BitsPerPixel`（偏移 `0x19`）
- ☑ 设置视频模式：`INT $0x10 AX=0x4F02 BX=0x4118`（bit14=1 启用 linear framebuffer）
- ☑ 将 `{PhysBasePtr, BytesPerScanLine, XResolution, YResolution, BitsPerPixel}` 打包写入 `0x6400`（16 字节），供后续填入 `BootInfo`；QEMU 典型值：`PhysBasePtr=0xFD000000, pitch=4096, 1024×768, 32bpp`

---

### `002_boot_gdt_protected`
**效果**：QEMU 不崩溃，debugcon 输出 `P`（单字节确认进入保护模式）

> **验证手段**：QEMU debug port `0xE9`（`-debugcon stdio`），单条 `outb $0x50, $0xE9` 输出字符 `P`，无需任何初始化，不碰串口

- ☑ `boot/stage2.S` 定义 `gdt_table`（8 字节对齐）：null=`0`，code32=`0x00CF9A000000FFFF`，data32=`0x00CF92000000FFFF`
- ☑ 定义 `gdt_desc`：`.word limit` + `.long base`
- ☑ `cli` → `lgdt gdt_desc` → `orl $0x1, %cr0` → `ljmp $0x08, $pm_entry`
- ☑ `.code32` 入口：设置 `%ds %es %ss %fs %gs = 0x10`，`movl $0x90000, %esp`
- ☑ 进入 `.code32` 后：`movb $0x50, %al; outb %al, $0xE9`（输出字符 `P` 到 QEMU debugcon 确认进入保护模式）
- ☑ QEMU 启动参数加 `-debugcon stdio`，验证终端出现 `P`

---

### `003_boot_long_mode`
**效果**：QEMU 不崩溃，debugcon 输出 `L`（确认进入 64-bit long mode）

> **验证手段**：同上，进入 `.code64` 后一条 `outb $0x4C, $0xE9` 输出字符 `L`，不碰串口

- ☑ 在物理地址 `0x1000–0x3FFF` 建临时页表：`rep stosl` 清零三页
- ☑ 写 PML4[0]=`PDPT|0x03`，PDPT[0]=`PD|0x03`，PD[0..3]=`i*0x200000|0x83`（PS=1，2MB 大页）
- ☑ `movl $0x1000, %cr3`
- ☑ `orl $0x20, %cr4`（PAE）
- ☑ `rdmsr`/`wrmsr` 对 `EFER(0xC0000080)` 置 `LME(bit8)`
- ☑ `orl $0x80000001, %cr0`（PG+PE）
- ☑ GDT 追加 64-bit 代码段描述符：`0x00AF9A000000FFFF`（L=1, D=0），偏移 `0x18`
- ☑ `ljmp $0x18, $lm_entry` 进入 `.code64`
- ☑ `.code64` 内：重新 `lgdt gdt64_desc`（base 改为 `.quad`），设段寄存器，`movabsq $stack_top, %rsp`
- ☑ 进入 `.code64` 后：`movb $0x4C, %al; outb %al, $0xE9`（输出字符 `L` 确认 long mode）
