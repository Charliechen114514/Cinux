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


## Phase 2 · 小内核 (Bootstrap Kernel)

> **架构说明**：小内核是最小化的 C++ 内核，只实现运行大内核所需的基础功能。
> **小内核职责**：初始化硬件 → 提供基础服务（输出、内存、磁盘）→ 加载并跳转到**大内核**。
> **目录**：`kernel/mini/`，隶属于大内核 `kernel/` 的目录支下。
> **内存布局**：小内核 @ 0x20000 (128KB)，大内核 @ 0x1000000 (16MB)。

### `005_mini_kernel_entry`
**效果**：串口输出 `[MINI] Bootstrap kernel running @ 0x20000`

- ☑ `kernel/mini/drivers/serial.hpp/cpp`：`Serial::init(port,baud)`，`Serial::putc(c)`，`Serial::puts(s)`
- ☑ `kernel/mini/lib/kprintf.hpp/cpp`：简化版 `kvprintf`/`kprintf`，支持 `%d %u %x %X %s %p %c %%`
- ☑ `kernel/mini/main.cpp`：`mini_kernel_main(BootInfo*)` 调 `Serial::init()` → `kprintf("[MINI] Bootstrap kernel running @ 0x20000\n")`
- ☑ 完成项目的调试基建和测试基建

### `006_mini_kernel_pmm`
**效果**：物理内存分配器工作，输出 `[MINI] PMM: Total XMB, Free XMB`

- ☑ `kernel/mini/mm/pmm.hpp/cpp`：Bitmap 物理内存分配器
  - `init(BootInfo&)`：解析 E820，初始化 bitmap
  - `alloc_page() → uint64_t`（返回物理地址，0=OOM）
  - `free_page(uint64_t phys)`
  - `free_page_count()` / `total_page_count()`
- ☑ Bitmap 放在小内核末端（`__mini_kernel_end` 对齐后）
- ☑ 过滤低 1MB，标记小内核自身和 bitmap 为已用
- ☑ `mini_kernel_main` 输出：`kprintf("[MINI] PMM: Total %dMB, Free %dMB\n", ...)`

### `007_mini_kernel_interrupts`
**效果**：触发异常不死机，能看到错误信息

- ☑ `kernel/mini/arch/x86_64/gdt.hpp/cpp`：基础 GDT（null/code64/data64 三项即可）
- ☑ `kernel/mini/arch/x86_64/idt.hpp/cpp`：简化版 IDT（只配置必要向量）
- ☑ `kernel/mini/arch/x86_64/interrupts.S`：仅 #BP(3) 和 #PF(14) 的 ISR stub
- ☑ `kernel/mini/arch/x86_64/exception_handlers.cpp`：
  - `handle_bp(InterruptFrame*)`：打印断点异常信息和寄存器 dump
  - `handle_pf(InterruptFrame*)`：读取 CR2，解析页错误码，打印详细信息
- ☑ `mini_kernel_main` 测试：`asm volatile("int $3")` 验证输出

---
### `008_mini_kernel_disk_and_loader`
**效果**：从磁盘加载大内核 ELF 并跳转

> **加载格式说明**：小内核→大内核使用 ELF 格式，因为小内核有完整的 C++ 运行环境和内存管理，处理 ELF 重定位更可靠；bootloader→小内核用 bin 是为了简化阶段 2 代码

- ☑ `kernel/mini/drivers/ata.hpp/cpp`：ATA PIO 磁盘驱动
  - `init()`：检测并初始化 ATA 控制器
  - `read(uint64_t lba, uint16_t count, void* buffer)`：读取扇区
- ☑ `kernel/mini/elf_loader.hpp/cpp`：ELF64 解析器
  - `parse_elf_header(void* elf) → bool`：验证魔数和架构
  - `calculate_kernel_size(Elf64_Ehdr*) → size_t`：遍历 PT_LOAD
  - `load_elf(void* elf_src, uint64_t load_base) → uint64_t`：返回 entry_point，处理 PT_LOAD 段搬运和 BSS 清零
- ☑ `kernel/mini/big_kernel_loader.hpp/cpp`：
  - `load_big_kernel(uint64_t disk_lba) → uint64_t`：循环读取大内核到 `0x1000000`（16MB）
- ☑ `mini_kernel_main` 流程：
  1. 初始化串口
  2. 初始化 PMM
  3. 初始化 IDT（#BP/#PF）
  4. 初始化 ATA
  5. 加载大内核：`entry = load_big_kernel(BIG_KERNEL_LBA)`
  6. 输出 `[MINI] Jumping to big kernel at 0x...`
  7. 跳转：`movq entry, %rax; jmp *%rax`

**常量定义**：
```cpp
constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;
constexpr uint64_t BIG_KERNEL_LOAD_ADDR  = 0x1000000;   // 16MB
```

---

## Phase 3 · 大内核基础设施

> **架构说明**：大内核从 `kernel/` 目录构建，由小内核加载并跳转，实现完整的 OS 功能。

### `009_big_kernel_entry`
**效果**：串口输出 `[BIG] Big kernel running @ 0x1000000`

- ☑ `kernel/linker.ld`：`KERNEL_VMA = 0xFFFFFFFF80000000`，`.text` AT(VMA-KERNEL_VMA)`，加载地址 `0x1000000`
- ☑ `kernel/arch/x86_64/boot.S`：`_start` 切换 `%rsp` 到 `__kernel_stack_top`，`xorq %rbp,%rbp`，`rep stosb` 清 BSS，`call _init_global_ctors`，`call kernel_main`，`.halt: cli; hlt`
- ☑ `kernel/arch/x86_64/crt_stub.cpp`：`__cxa_pure_virtual`、`__stack_chk_fail`（均 `cli;hlt`），`__cxa_atexit` 返回 0，`_init_global_ctors` 遍历 `.init_array`
- ☑ 链接脚本加 `.init_array` 段，收集全局构造器
- ☑ `kernel/arch/x86_64/io.hpp`：`io_inb/io_outb/io_inw/io_outw/io_inl/io_outl/io_wait` 全部内联汇编，`"memory"` clobber
- ☑ `kernel/drivers/serial.hpp/cpp`：`Serial::init(port,baud)`，`Serial::putc(c)`，`Serial::puts(s)`，`Serial::is_ready()`
- ☑ `kernel/lib/kprintf.hpp/cpp`：`kvprintf(fmt,va_list)`，`kprintf(fmt,...)`，`kpanic(fmt,...) [[noreturn]]`，支持 `%d %u %x %X %s %p %c %%`，`%p` 输出 16 位十六进制
- ☑ `kernel_main(BootInfo*)` 调 `Serial::init()` → `kprintf("[BIG] Big kernel running @ 0x1000000\n")`
- ☑ host 端单元测试 `tests/unit/test_kprintf.cpp`：mock `Serial::putc`，验证各格式化输出

### `010_big_kernel_gdt_idt`
**效果**：触发除零异常后串口打印寄存器 dump，不死机

- ☐ `kernel/arch/x86_64/gdt.hpp`：`GDTEntry [[gnu::packed]]`，`constexpr make_null/make_code64/make_data64/make_tss`；选择子常量 `GDT_KERNEL_CODE=0x08`，`GDT_KERNEL_DATA=0x10`，`GDT_USER_CODE=0x1B`，`GDT_USER_DATA=0x23`，`GDT_TSS=0x28`
- ☐ `kernel/arch/x86_64/gdt.cpp`：全局 GDT 数组含 null/kernel_code64/kernel_data64/user_code64/user_data64/TSS（16字节双槽），`gdt_init()` 填充 + `lgdt` + `ltr $GDT_TSS`
- ☐ `kernel/arch/x86_64/idt.hpp`：`IDTEntry [[gnu::packed]]`，`InterruptFrame` 结构（`r15..rax + vector + error_code + rip/cs/rflags/rsp/ss`），`using IRQHandler = void(*)(InterruptFrame*)`，`idt_set_handler(vector, handler)`，`idt_init()`
- ☐ `kernel/arch/x86_64/interrupts.S`：`.macro isr_noerr` 推 `$0` + vec，`.macro isr_err` 推 vec；`isr_common` 保存 r15..rax，`movq %rsp,%rdi`，`call isr_dispatch`，恢复寄存器，`addq $16,%rsp`，`iretq`；用宏批量生成 256 个 stub（8/10/11/12/13/14/17/21 有 error code）
- ☐ `kernel/arch/x86_64/exception_handlers.cpp`：`dump_registers(InterruptFrame*)` 格式化输出所有寄存器；`handle_pf`（读 `%cr2`）、`handle_gp`、`handle_df [[noreturn]]`；在 `idt_init()` 中注册
- ☐ `kernel_main` 中 `asm volatile("int $3")` 触发 `#BP` 验证 dump 输出

---

### `011_big_kernel_pic_irq`
**效果**：串口每秒输出 `[TICK] uptime: Ns`

- ☐ `kernel/arch/x86_64/pic.hpp/cpp`：`PIC::init(master_offset=0x20, slave_offset=0x28)` 发 ICW1–ICW4（含 `io_wait()`），重映射 IRQ0-7→0x20-0x27，IRQ8-15→0x28-0x2F；`PIC::send_eoi(irq)`，`PIC::mask(irq)`，`PIC::unmask(irq)`，`PIC::disable_all()`
- ☐ `kernel/drivers/pit.hpp/cpp`：`PIT::init(freq_hz=100)` 写 CMD `0x43=0x36`，写 divisor=`1193182/freq_hz` 低/高字节到 `0x40`；全局 `tick_count`；`PIT::get_ticks()`，`PIT::get_uptime_ms()`
- ☐ `PIT::irq0_handler(InterruptFrame*)` 递增 `tick_count`，每 `freq_hz` tick 调 `kprintf("[TICK] uptime: %us\n", ...)`;末尾 `PIC::send_eoi(0)`
- ☐ `idt_init()` 注册 `irq0_handler` 到 vector `0x20`；`kernel_main` 末尾 `PIC::init()`，`PIC::unmask(0)`，`sti`，死循环

---

## Phase 4 · 驱动三件套

### `012_driver_serial`
**效果**：`ctest` host 端 kprintf 测试全绿，串口输出格式验证字符串

- ☑ `kprintf` 补全：`fmt_uint(val,base,width,pad,upper,buf,len)` 支持前补零；`%08x`/`%-10s` 等宽度修饰；`%p` 输出 `0x` + 16 位十六进制
- ☑ host 测试 `test_kprintf.cpp`：mock `Serial::putc` 捕获输出，`TEST` 覆盖 `%d`/`%u`/`%x`/`%X`/`%s`/`%p`/`%%`/负数/前补零

### `013_driver_vga_fb`
**效果**：屏幕显示彩色字符，自动滚屏；kprintf 多后端架构可扩展

- ☑ **kprintf 多后端重构** `kernel/lib/kprintf.hpp/cpp`：定义 `using OutputSink = void(*)(char, void* ctx)`；`kprintf_register_sink(OutputSink, void* ctx)` 注册后端（最多 8 个）；内部维护 `Sink{fn, ctx, enabled}` 数组；`kprintf/kvprintf` 遍历所有 enabled sink 调用，保留 `vkprintf_impl` 模板不变；`kprintf_init()` 默认注册 serial sink
- ☑ `kernel/drivers/video/framebuffer.hpp/cpp`：`Framebuffer {addr,width,height,pitch,bpp}`；`init(BootInfo&)`（映射 MMIO）；`put_pixel(x,y,argb)`=`addr[y*pitch/4+x]=color`；`fill_rect`；`scroll_up(lines,line_height)`；`clear(color=0)`；`get_pixel(x,y)` 用于测试 readback
- ☑ `assets/font.psf`：嵌入 PSF2 字体（8x16，256 glyphs）；用 `.incbin` 汇编嵌入（`font_data.S`），导出 `font_psf_start/end/size`
- ☑ `kernel/drivers/video/font.hpp/cpp`：解析 `PSFFont` header（magic=`0x864AB572`，width/height/bytes_per_glyph）；`font_render_char(fb,c,x,y,fg,bg)` 按位图逐 bit 渲染
- ☑ `kernel/drivers/video/console.hpp/cpp`：`Console {fb_,col_,row_,cols_,rows_,fg_,bg_}`；`putc(c)` 处理 `\n \r \b` 和 auto-newline；`scroll()` 调 `fb_.scroll_up`；`set_color`；`clear`；提供 `static void console_sink_adapter(char c, void* ctx)` 供 kprintf 注册
- ☑ `kernel_main` 中 console 初始化后调 `kprintf_register_sink(Console::console_sink_adapter, &console)` 完成双输出注册
- ☑ 驱动目录重组：`drivers/serial/`、`drivers/pit/`、`drivers/video/`
- ☑ Bootloader VBE 模式修正：0x118(24bpp) → 0x144(1024x768x32bpp)
- ☑ QEMU in-kernel 测试 `test_video.cpp`：fb init/readback、font rendering、console putc/clear
- ☑ Host 测试：`test_font.cpp`、`test_console.cpp`、`test_framebuffer.cpp`

### `014_driver_keyboard`
**效果**：按键后屏幕和串口同步回显字符

- ☑ `keyboard_init()`：禁用 PS/2 设备（CMD `0xAD/0xA7`），刷新输出缓冲，写配置字节（IRQ1 on，mouse IRQ12 off），控制器自检（CMD `0xAA`，期望 `0x55`），重新启用（CMD `0xAE`）
- ☑ `sc_to_ascii_lower[128]` + `sc_to_ascii_upper[128]`：扫描码集 1 → ASCII 查找表（`0x1E='a'`，`0x02='1'`，等）
- ☑ `KeyEvent {ascii, scancode, pressed, shift, ctrl, alt}`；维护 modifier 状态（break code = makecode|0x80）
- ☑ 环形队列 `key_queue[64]`，head/tail；`keyboard_irq1_handler` 读 `0x60` enqueue，发 `PIC::send_eoi(1)`
- ☑ `keyboard_poll(KeyEvent& out)` dequeue
- ☑ `idt_init()` 注册 IRQ1 handler（vector `0x21`）；`kernel_main` 循环 `keyboard_poll` → `Console::putc`
- ☑ host 测试 `test_keyboard.cpp`：验证扫描码转换表（`0x1E→'a'`，shift+`0x1E→'A'`，break code→released）