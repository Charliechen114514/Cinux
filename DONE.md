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

### `004_boot_load_mini_kernel`
**效果**：QEMU 不崩溃，debugcon 输出 `J`（确认即将跳转小内核），随后小内核接管

> **验证手段**：`jmp *%rax` 前一条 `outb $0x4A, $0xE9` 输出字符 `J`；小内核启动后由 `005` 的串口驱动接管所有后续输出
> **加载方案**：real mode 直接读取完整小内核到目标地址（无需 protected mode 处理）
> **小内核物理加载地址**：`0x20000`（128KB，在 real mode 可访问范围内）
> **格式说明**：小内核使用扁平二进制（bin）格式，objcopy 从 ELF 转换而来

#### 004_boot_load_mini_kernel_A：real mode 内完成（在 001 末尾 VESA 之后、进保护模式之前）

- ☑ E820 内存枚举：`INT $0x15 AX=0xE820`，每次填一条 `MemoryMapEntry` 到 `0x5000` 起的缓冲（最多 32 条），记录条目数到 `0x5000` 前 4 字节
- ☑ 磁盘读 ELF header：`INT $0x13 AH=0x42`，DAP 指定 LBA=`MINI_KERNEL_LBA`（build_image.sh 写死，例如 16），sectors=8（4KB），dest=`0x1000:0x0000`（物理 `0x10000`）；仅用于后续解析 PHDR 获取小内核总大小，**支持任意大小的内核镜像**


#### 004_boot_load_mini_kernel_B：real mode 加载完成，protected mode 无操作

- ☑ 定义 `boot/boot_info.h`（bootloader 和内核共用，带汇编注释说明设计原因）：
  ```c
  typedef struct { uint64_t base, length; uint32_t type, _pad; } MemoryMapEntry;
  typedef struct {
      uint64_t entry_point, kernel_phys_base, kernel_size;
      uint64_t fb_addr; uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
      uint32_t mmap_count; MemoryMapEntry mmap[32];
  } BootInfo;
  ```
- ☑ `boot/common/boot.S` 的 `load_kernel_from_disk()` 已完成加载（real mode 读 LBA 16 → 0x20000）
- ☑ `stage2.S` 保护模式入口：无操作，直接跳过进入 long mode

#### 004_boot_load_mini_kernel_C：long mode 内填 BootInfo 并跳转

- ☑ 进入 `.code64` 后填充 `BootInfo`（约定放物理 `0x7000`）：
  - `entry_point`：高半核虚拟地址 `0xFFFFFFFF80020000`（固定值，`0xFFFFFFFF80000000 + 0x20000`）
  - `kernel_phys_base = 0x20000`，`kernel_size` = 写死或从磁盘布局计算
  - `fb_addr/width/height/pitch/bpp`：从 `0x6400` 读取（001 阶段 VESA 保存的值）
  - `mmap_count` + `mmap[]`：从 `0x5000` 读取（E820 保存的值）
- ☑ `kernel/mini/linker.ld`：输出格式 `elf64-x86-64`，物理地址 `0x20000`，编译后用 `objcopy -O binary` 转成 `mini.bin`
- ☑ `scripts/build_image.sh`：sector 0 = MBR，sector 1–15 = stage2，sector 16+ = mini.bin（小内核扁平二进制）
- ☑ 跳转前：`movb $0x4A, %al; outb %al, $0xE9`（debugcon 输出 `J`）
- ☑ `movq $0x7000, %rdi`（BootInfo* 第一参数），`movq $0xFFFFFFFF80020000, %rax`，`jmp *%rax`

##### 🎉 额外完成（超出预期）

- ☑ **C++ 运行时支持**：`kernel/mini/arch/x86_64/crt_stub.cpp`
  - `__cxa_pure_virtual` - 纯虚函数调用处理
  - `__stack_chk_fail` - 栈保护失败处理
  - `__cxa_atexit` - atexit 处理（空实现）
  - `_init_global_ctors` - 全局构造函数初始化
  - `operator new/delete` - freestanding 内存管理（halt 实现）
- ☑ **C++ 面向对象特性验证**：
  - 类构造函数/析构函数正常工作
  - 虚函数和 vtable 多态机制完整
  - 全局对象在 `_init_global_ctors` 中正确构造
  - 验证输出：`===CPPGC1V123B===END`
- ☑ **关键 Bug 修复**：
  - **BSS 清除破坏参数**：在清除前保存 `%rdi` 到 `.data` 段
  - **符号地址冲突**：`__boot_info_ptr` 从 `.bss` 移到 `.data` 避免与 C++ 全局变量冲突
- ☑ **调试记录**：`document/notes/006/`
  - `boot_info_param_corruption.md` - BootInfo 参数被破坏的调试过程
  - `bss_data_symbol_conflict.md` - .bss/.data 符号冲突问题分析
