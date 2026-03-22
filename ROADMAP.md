# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☐` 打完后打 tag，触发 prompts/ 工作流

---

## Phase 1 · Bootloader

### `001_boot_real_mode`
**效果**：QEMU 图形窗口左上角出现 `Cinux Booting...`，串口出现 `Stage2 OK`

- ☐ `boot/mbr.S`：`.code16`，`ljmp $0,$real_start` 规范化 CS，清零 `%ds %es %ss %sp`，`movb %dl, boot_drive` 保存启动盘号
- ☐ 实现 `print_string`：`lodsb` + BIOS `INT $0x10 AH=0x0E` 循环输出，以 `\0` 结尾
- ☐ 打印 `msg: .asciz "Cinux Booting..."`
- ☐ 末尾加 DAP 结构（`dap`）：size=0x10，sectors=4，dest=`0x0000:0x8000`，LBA=1
- ☐ 调 `INT $0x13 AH=0x42`（扩展磁盘读）将 stage2 载入 `0x8000`
- ☐ `ljmp $0, $0x8000` 跳转 stage2
- ☐ `boot/stage2.S`：入口打印 `Stage2 OK`，后续步骤在此文件继续
- ☐ `scripts/build_image.sh` 更新：sector 0 写 MBR，sector 1+ 写 stage2.bin
- ☐ 开启 A20：`INT $0x15 AX=0x2401`

---

### `002_boot_gdt_protected`
**效果**：串口输出 `[PM] Protected mode OK`

- ☐ `boot/stage2.S` 定义 `gdt_table`（8 字节对齐）：null=`0`，code32=`0x00CF9A000000FFFF`，data32=`0x00CF92000000FFFF`
- ☐ 定义 `gdt_desc`：`.word limit` + `.long base`
- ☐ `cli` → `lgdt gdt_desc` → `orl $0x1, %cr0` → `ljmp $0x08, $pm_entry`
- ☐ `.code32` 入口：设置 `%ds %es %ss %fs %gs = 0x10`，`movl $0x90000, %esp`
- ☐ 实现 `serial_init`：COM1 `0x3F8`，波特率 115200（divisor=1），8N1，DLAB 流程
- ☐ 实现 `serial_putc`：轮询 `0x3FD` THRE 位（bit5），`outb %al, $0x3F8`
- ☐ 实现 `serial_puts`：循环调 `serial_putc`
- ☐ 进保护模式后调 `serial_init`，输出 `[PM] Protected mode OK\r\n`

---

### `003_boot_long_mode`
**效果**：串口输出 `[LM] Long mode OK`

- ☐ 在物理地址 `0x1000–0x3FFF` 建临时页表：`rep stosl` 清零三页
- ☐ 写 PML4[0]=`PDPT|0x03`，PDPT[0]=`PD|0x03`，PD[0..3]=`i*0x200000|0x83`（PS=1，2MB 大页）
- ☐ `movl $0x1000, %cr3`
- ☐ `orl $0x20, %cr4`（PAE）
- ☐ `rdmsr`/`wrmsr` 对 `EFER(0xC0000080)` 置 `LME(bit8)`
- ☐ `orl $0x80000001, %cr0`（PG+PE）
- ☐ GDT 追加 64-bit 代码段描述符：`0x00AF9A000000FFFF`（L=1, D=0），偏移 `0x18`
- ☐ `ljmp $0x18, $lm_entry` 进入 `.code64`
- ☐ `.code64` 内：重新 `lgdt gdt64_desc`（base 改为 `.quad`），设段寄存器，`movabsq $stack_top, %rsp`
- ☐ 串口输出 `[LM] Long mode OK`

---

### `004_boot_load_kernel`
**效果**：串口依次出现 `[BOOT] Loading kernel...` → `[BOOT] Entry: 0xFFFF...` → `[BOOT] Jumping...`

- ☐ 定义 `boot/boot_info.h`：`MemoryMapEntry {base, length, type}`，`BootInfo {entry_point, kernel_phys_base, kernel_size, fb_addr/width/height/pitch/bpp, mmap_count, mmap[32]}`
- ☐ 在 long mode 前（保护模式内）用 `INT $0x13 AH=0x42` 读内核 ELF 到物理 `0x100000`（1MB）
- ☐ 用 E820（`INT $0x15 AX=0xE820`）枚举内存图，填入 `BootInfo.mmap`
- ☐ 实现 `elf64_load()`：验证魔数 `\x7FELF` + `e_machine=0x3E`，遍历 `PT_LOAD` 段，按 `p_paddr` 复制，多余 BSS 清零，返回 `e_entry`
- ☐ 在临时页表 PML4[511] 建高半核映射：`0xFFFFFFFF80000000` → `0x000000`（Identity 基础上加高半段）
- ☐ 填充 `BootInfo`，地址约定放 `0x7000`
- ☐ `movq $boot_info_addr, %rdi`（第一参数），`jmp *%rax`（内核入口，用 `jmp` 不用 `call`）

---

## Phase 2 · 内核基础设施

### `005_kernel_entry`
**效果**：串口输出 `[KERNEL] Hello from Cinux Kernel!` 和内存统计

- ☐ `kernel/linker.ld`：`KERNEL_VMA = 0xFFFFFFFF80000000`，`.text` AT(VMA-KERNEL_VMA)，`.bss` 含 `__bss_start/__bss_end`，`__kernel_end`，16KB 内核栈
- ☐ `kernel/arch/x86_64/boot.S`：`_start` 切换 `%rsp` 到 `__kernel_stack_top`，`xorq %rbp,%rbp`，`rep stosb` 清 BSS，`call _init_global_ctors`，`call kernel_main`，`.halt: cli; hlt`
- ☐ `kernel/arch/x86_64/crt_stub.cpp`：`__cxa_pure_virtual`、`__stack_chk_fail`（均 `cli;hlt`），`__cxa_atexit` 返回 0，`_init_global_ctors` 遍历 `.init_array`
- ☐ 链接脚本加 `.init_array` 段，收集全局构造器
- ☐ `kernel/arch/x86_64/io.hpp`：`io_inb/io_outb/io_inw/io_outw/io_inl/io_outl/io_wait` 全部内联汇编，`"memory"` clobber
- ☐ `kernel/drivers/serial.hpp/cpp`：`Serial::init(port,baud)`，`Serial::putc(c)`，`Serial::puts(s)`，`Serial::is_ready()`
- ☐ `kernel/lib/kprintf.hpp/cpp`：`kvprintf(fmt,va_list)`，`kprintf(fmt,...)`，`kpanic(fmt,...) [[noreturn]]`，支持 `%d %u %x %X %s %p %c %%`，`%p` 输出 16 位十六进制
- ☐ `kernel_main(BootInfo*)` 调 `Serial::init()` → `kprintf("[KERNEL] Hello from Cinux Kernel!\n")`
- ☐ host 端单元测试 `test/unit/test_kprintf.cpp`：mock `Serial::putc`，验证各格式化输出

---

### `006_kernel_gdt_idt`
**效果**：触发除零异常后串口打印寄存器 dump，不死机

- ☐ `kernel/arch/x86_64/gdt.hpp`：`GDTEntry [[gnu::packed]]`，`constexpr make_null/make_code64/make_data64/make_tss`；选择子常量 `GDT_KERNEL_CODE=0x08`，`GDT_KERNEL_DATA=0x10`，`GDT_USER_CODE=0x1B`，`GDT_USER_DATA=0x23`，`GDT_TSS=0x28`
- ☐ `kernel/arch/x86_64/gdt.cpp`：全局 GDT 数组含 null/kernel_code64/kernel_data64/user_code64/user_data64/TSS（16字节双槽），`gdt_init()` 填充 + `lgdt` + `ltr $GDT_TSS`
- ☐ `kernel/arch/x86_64/idt.hpp`：`IDTEntry [[gnu::packed]]`，`InterruptFrame` 结构（`r15..rax + vector + error_code + rip/cs/rflags/rsp/ss`），`using IRQHandler = void(*)(InterruptFrame*)`，`idt_set_handler(vector, handler)`，`idt_init()`
- ☐ `kernel/arch/x86_64/interrupts.S`：`.macro isr_noerr` 推 `$0` + vec，`.macro isr_err` 推 vec；`isr_common` 保存 r15..rax，`movq %rsp,%rdi`，`call isr_dispatch`，恢复寄存器，`addq $16,%rsp`，`iretq`；用宏批量生成 256 个 stub（8/10/11/12/13/14/17/21 有 error code）
- ☐ `kernel/arch/x86_64/exception_handlers.cpp`：`dump_registers(InterruptFrame*)` 格式化输出所有寄存器；`handle_pf`（读 `%cr2`）、`handle_gp`、`handle_df [[noreturn]]`；在 `idt_init()` 中注册
- ☐ `kernel_main` 中 `asm volatile("int $3")` 触发 `#BP` 验证 dump 输出

---

### `007_pic_irq`
**效果**：串口每秒输出 `[TICK] uptime: Ns`

- ☐ `kernel/arch/x86_64/pic.hpp/cpp`：`PIC::init(master_offset=0x20, slave_offset=0x28)` 发 ICW1–ICW4（含 `io_wait()`），重映射 IRQ0-7→0x20-0x27，IRQ8-15→0x28-0x2F；`PIC::send_eoi(irq)`，`PIC::mask(irq)`，`PIC::unmask(irq)`，`PIC::disable_all()`
- ☐ `kernel/drivers/pit.hpp/cpp`：`PIT::init(freq_hz=100)` 写 CMD `0x43=0x36`，写 divisor=`1193182/freq_hz` 低/高字节到 `0x40`；全局 `tick_count`；`PIT::get_ticks()`，`PIT::get_uptime_ms()`
- ☐ `PIT::irq0_handler(InterruptFrame*)` 递增 `tick_count`，每 `freq_hz` tick 调 `kprintf("[TICK] uptime: %us\n", ...)`;末尾 `PIC::send_eoi(0)`
- ☐ `idt_init()` 注册 `irq0_handler` 到 vector `0x20`；`kernel_main` 末尾 `PIC::init()`，`PIC::unmask(0)`，`sti`，死循环

---

## Phase 3 · 驱动三件套

### `008_driver_serial`
**效果**：`ctest` host 端 kprintf 测试全绿，串口输出格式验证字符串

- ☐ `kprintf` 补全：`fmt_uint(val,base,width,pad,upper,buf,len)` 支持前补零；`%08x`/`%-10s` 等宽度修饰；`%p` 输出 `0x` + 16 位十六进制
- ☐ host 测试 `test_kprintf.cpp`：mock `Serial::putc` 捕获输出，`TEST` 覆盖 `%d`/`%u`/`%x`/`%X`/`%s`/`%p`/`%%`/负数/前补零

---

### `009_driver_vga_fb`
**效果**：屏幕显示彩色字符，自动滚屏

- ☐ `kernel/drivers/framebuffer.hpp/cpp`：`Framebuffer {addr,width,height,pitch,bpp}`；`init(BootInfo&)`（映射 MMIO）；`put_pixel(x,y,argb)`=`addr[y*pitch/4+x]=color`；`fill_rect`；`scroll_up(lines,line_height)` 用 `memmove`；`clear(color=0)`
- ☐ `assets/font.psf`：嵌入 PSF2 字体；CMake 用 `objcopy --input binary --output elf64-x86-64` 转 `.o`，导出 `_binary_font_psf_start/end/size`
- ☐ `kernel/drivers/font.hpp/cpp`：解析 `PSFFont` header（magic=`0x864AB572`，width/height/bytes_per_glyph）；`font_render_char(fb,c,x,y,fg,bg)` 按位图逐 bit 渲染
- ☐ `kernel/drivers/console.hpp/cpp`：`Console {fb_,col_,row_,cols_,rows_,fg_,bg_}`；`putc(c)` 处理 `\n \r \b` 和 auto-newline；`scroll()` 调 `fb_.scroll_up`；`set_color`；`clear`
- ☐ `kprintf` 输出同时调 `Console::putc`（若 console 已初始化）

---

### `010_driver_keyboard`
**效果**：按键后屏幕和串口同步回显字符

- ☐ `keyboard_init()`：禁用 PS/2 设备（CMD `0xAD/0xA7`），刷新输出缓冲，写配置字节（IRQ1 on，mouse IRQ12 off），控制器自检（CMD `0xAA`，期望 `0x55`），重新启用（CMD `0xAE`）
- ☐ `sc_to_ascii_lower[128]` + `sc_to_ascii_upper[128]`：扫描码集 1 → ASCII 查找表（`0x1E='a'`，`0x02='1'`，等）
- ☐ `KeyEvent {ascii, scancode, pressed, shift, ctrl, alt}`；维护 modifier 状态（break code = makecode|0x80）
- ☐ 环形队列 `key_queue[64]`，head/tail；`keyboard_irq1_handler` 读 `0x60` enqueue，发 `PIC::send_eoi(1)`
- ☐ `keyboard_poll(KeyEvent& out)` dequeue
- ☐ `idt_init()` 注册 IRQ1 handler（vector `0x21`）；`kernel_main` 循环 `keyboard_poll` → `Console::putc`
- ☐ host 测试 `test_keyboard.cpp`：验证扫描码转换表（`0x1E→'a'`，shift+`0x1E→'A'`，break code→released）

---

## Phase 4 · 内存管理

### `011_mm_pmm`
**效果**：串口输出内存统计，分配/释放测试通过

- ☐ `parse_memory_map(BootInfo&, regions[], max)` 提取 type=1 可用区域，过滤低 1MB，对齐到 4KB
- ☐ `kernel/mm/pmm.hpp`：`PMM::init(BootInfo&)`，`alloc_page()→uint64_t`（0=OOM），`free_page(phys)`，`alloc_pages(count)`，`free_pages(phys,count)`，`free_page_count()`，`total_page_count()`
- ☐ Bitmap 放于 `__kernel_end` 对齐后；先全标记已用，再将可用区域清零，再标记内核+bitmap 自身为已用
- ☐ `alloc_page` 用 `__builtin_ctzll` 加速 bit 扫描（64 位一组）
- ☐ `kprintf` 输出统计：`[PMM] Total: XMB, Free: XMB`
- ☐ host 测试 `test_pmm.cpp`：mock 内存图，1000 次 alloc/free，验证计数正确、地址 4KB 对齐

---

### `012_mm_vmm`
**效果**：map/unmap/translate 可用，缺页异常正确处理

- ☐ `kernel/arch/x86_64/paging.hpp`：`union PageEntry {uint64_t raw; struct{present,writable,user,pwt,pcd,accessed,dirty,huge,global,_avail,addr:40,nx}}`；`FLAG_PRESENT/WRITABLE/USER/NX` 常量
- ☐ `kernel/mm/vmm.hpp`：`VMM::map(virt,phys,flags,pml4=nullptr)`（缺中间级时 `PMM::alloc_page()` 新建并清零），`VMM::unmap(virt,pml4=nullptr)`，`VMM::translate(virt)→uint64_t`，`flush_tlb(virt)`（`invlpg`），`flush_tlb_all()`（reload CR3）
- ☐ `#PF handler` 更新：读 `%cr2`，尝试按需分配，无法处理时 panic
- ☐ host 测试 `test_vmm.cpp`：mock PMM，验证 map→translate→unmap 正确

---

### `013_mm_heap`
**效果**：`kmalloc`/`kfree` 可用，`new`/`delete` 接管，碎片化测试通过

- ☐ `BlockHeader [[gnu::packed]] {magic=0xDEADBEEF, size, free, _pad[7], *next}`
- ☐ `kernel/mm/heap.hpp`：`Heap::init(virt_base, initial_size)`，`alloc(size,align=16)→void*`（first-fit，split），`free(ptr)`（magic 验证，coalesce），`dump_stats()`
- ☐ free list 耗尽时调 `VMM::map()` 扩展堆
- ☐ `operator new/new[]/delete/delete[]` 接管；`operator new(size, align_val_t)` 支持对齐分配
- ☐ host 测试：1000 次随机大小 alloc/free，检查无泄漏；double-free 触发 magic 校验 panic

---

### `014_mm_address_space`
**效果**：独立地址空间创建/切换，用户区隔离验证

- ☐ `kernel/mm/address_space.hpp`：`class AddressSpace {pml4_phys_; static *kernel_pml4_}`；`static init_kernel()`（读 CR3 保存）；构造器（alloc 新 PML4，复制 PML4[256–511] 内核条目）；析构器（遍历用户区 PML4[0–255] 逐级释放）；`map/unmap/activate()`（mov CR3）
- ☐ 禁止拷贝构造和拷贝赋值（`= delete`）
- ☐ 串口验证：创建 AS#1 和 AS#2，在 AS#1 映射一页，切换到 AS#2，translate 该地址返回 0

---

## Phase 5 · 进程与调度

### `015_proc_context`
**效果**：两个内核线程交替串口打印，`yield` 切换

- ☐ `kernel/proc/process.hpp`：`enum TaskState {Running,Ready,Blocked,Dead}`；`struct alignas(16) CpuContext {r15,r14,r13,r12,rbp,rbx,rsp,rip}`；`struct Task {ctx,state,tid,priority,kernel_stack,kernel_stack_top,*addr_space,*name}`
- ☐ `Task::create_kernel_task(entry,name)`：`kmalloc` TCB，`PMM::alloc_pages(4)` 内核栈（16KB），初始化 `ctx.rsp`=栈顶，`ctx.rip`=入口，栈底写 magic 0xDEADC0DE
- ☐ `kernel/arch/x86_64/context_switch.S`：`context_switch(CpuContext* from, CpuContext* to)`，保存 callee-saved（r15,r14,r13,r12,rbp,rbx,rsp）+ `leaq .restore(%rip)→rip`，恢复 to 的对应字段，`jmp *56(%rsi)`
- ☐ `yield()` = 调度器选下一个 task，调 `context_switch(current, next)`
- ☐ `kernel_main` 创建两个任务各打印 5 行，手动 `yield`，验证交替输出

---

### `016_proc_scheduler`
**效果**：3 个线程无 `yield` 调用，时钟中断驱动交替运行

- ☐ `kernel/proc/sync.hpp`：`class Spinlock`，`acquire()`（`__atomic_test_and_set` + `pause`），`release()`（`__atomic_clear`），`[[nodiscard]] guard()`（RAII）
- ☐ `kernel/proc/scheduler.hpp`：`Scheduler::init()`，`add_task(Task*)`，`remove_task(Task*)`，`tick()`（IRQ0 调用），`yield()`，`block(Task*,reason)`，`unblock(Task*)`，`current()`，`is_initialized()`
- ☐ 就绪队列：环形数组 Round-Robin；`idle_task`（只执行 `hlt`）
- ☐ `tick()` 每 N tick 调 `schedule()`，`schedule()` 先发 EOI 再 `context_switch`
- ☐ 进程切换时更新 `TSS.RSP0` 为新 task 内核栈顶
- ☐ `per_cpu` 数据结构占位（`struct PerCPU {Task* current; uint64_t kernel_stack}`，单核用静态全局）

---

### `017_proc_sync`
**效果**：Mutex/Semaphore 可用，producer-consumer 无竞争

- ☐ `class Mutex {spin_, *owner_, *wait_head_}`：`lock()`（已锁则 block 当前 task），`unlock()`（唤醒等待队列头），`try_lock()`，`[[nodiscard]] guard()`
- ☐ `class Semaphore {spin_, count_, *wait_head_}`：`post()`（V，count++，唤醒），`wait()`（P，count--，负数则阻塞），`try_wait()`
- ☐ 演示任务：producer 写共享缓冲，consumer 读，Semaphore 同步，串口输出 `sent: 0,1,2,3,4` / `got: 0,1,2,3,4`

---

## Phase 6 · 用户态与系统调用

### `018_ring3_usermode`
**效果**：第一个用户态程序运行，执行特权指令触发 `#GP`

- ☐ `kernel/arch/x86_64/tss.hpp`：`TSS [[gnu::packed]] {_reserved0, rsp[3], _reserved1, ist[7], _reserved2, _reserved3, iopb_offset}`，16 字节对齐
- ☐ `tss_init()`：静态分配 TSS，IST1 指向独立 4KB Double Fault 栈，GDT TSS 描述符填基址+限长+type=0x89，`ltr $GDT_TSS`
- ☐ `tss_set_rsp0(kernel_stack_top)` 每次 task 切换调用
- ☐ `kernel/arch/x86_64/usermode.S`：`jump_to_usermode(entry,user_stack,arg)`，配置 `STAR MSR`，`%rcx=entry`，`%rsp=user_stack`，`%rdi=arg`，`%r11=RFLAGS|0x200`，`swapgs`，`sysretq`
- ☐ 用户态 ELF 链接到 `0x400000`；`AddressSpace` 分配用户栈（`0x7FFFFF000` 附近）
- ☐ 验证：用户程序执行 `cli`（特权指令）触发 `#GP`，串口输出 `[EXCEPTION] #GP at RIP=0x...`

---

### `019_syscall`
**效果**：用户态 `syscall` 指令触发内核打印 `[USER] Hello from Ring 3!`

- ☐ syscall 号常量：`SYS_read=0, SYS_write=1, SYS_exit=60, SYS_yield=24`
- ☐ `syscall_init()`：写 `LSTAR` MSR（入口地址），`SFMASK` MSR（至少清 IF），`STAR` MSR（内核/用户段选择子）
- ☐ `kernel/arch/x86_64/syscall.S`：`syscall_entry`，`swapgs`，切换到内核栈（`%gs:kernel_rsp`），保存 `%rcx/%r11` + arg 寄存器，`call syscall_dispatch`，恢复，切回用户栈，`swapgs`，`sysretq`
- ☐ `using SyscallFn = int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)`；`syscall_table[256]`
- ☐ `sys_write(fd,buf_virt,count,...)`：验证 `buf_virt < 0x800000000000`，fd=1 输出到串口+Console
- ☐ `sys_exit(code,...)`：task 标记 Dead，`Scheduler::yield()`
- ☐ `sys_yield(...)`：直接 `Scheduler::yield()`
- ☐ `user/libc/syscall.h`：`_syscall(nr,a,b,c)` 内联汇编封装；`write/exit/read` 宏

---

### `020_shell`
**效果**：用户态 shell，`echo`/`help`/`clear` 可用

- ☐ `user/libc/syscall.h` 完善：`read(fd,buf,len)` 封装
- ☐ `user/shell/main.cpp`：主循环 `print_prompt → read_line(sys_read) → tokenize → dispatch → repeat`
- ☐ tokenizer：按空格切割，返回 `argc/argv`
- ☐ builtin 表：`{"echo",cmd_echo},{"help",cmd_help},{"clear",cmd_clear},{nullptr,nullptr}`
- ☐ `cmd_echo`：`write(1, argv[1..], ...)`；`cmd_clear`：`write(1, "\033[2J\033[H", 7)`（ANSI 清屏）；`cmd_help`：打印命令列表
- ☐ shell 作为第一个用户态进程由 `kernel_main` 启动（`jump_to_usermode` 跳入）

---

## Phase 7 · 存储与文件系统

### `021_driver_ahci`
**效果**：串口输出 `[AHCI] Read sector 0: 55 AA`

- ☐ `kernel/drivers/pci.hpp/cpp`：`PCIDevice {bus,slot,func,vendor_id,device_id,class_code,subclass,prog_if,bar[6]}`；`pci_read/pci_write`（写 `0xCF8`，读 `0xCFC`）；`pci_find_ahci(out)`（枚举 class=0x01 subclass=0x06）
- ☐ `kernel/drivers/ahci.hpp/cpp`：`HBAmem [[gnu::packed]]`（cap/ghc/is/pi/...）；`HBAport [[gnu::packed]]`（clb/fb/is/ie/cmd/...）
- ☐ `ahci_init()`：映射 BAR5 MMIO（`VMM::map`），检测 `pi` 位图，为每活跃端口分配 Command List（32×32B）+ FIS Buffer（256B），物理连续且对齐
- ☐ `ahci_read(port,lba,count,buf)`：构造 CFIS（ATA READ DMA EXT=0x25）+ PRDT，写 `port.ci`，轮询 `port.is` 等待完成
- ☐ `ahci_write(port,lba,count,buf)`：同上，命令改为 ATA WRITE DMA EXT=0x35

---

### `022_fs_ramdisk`
**效果**：串口列出 initrd 中的文件名和大小

- ☐ `UstarHeader [[gnu::packed]]` 512 字节：`name[100]/mode[8]/uid/gid/size[12]/mtime[12]/checksum[8]/typeflag/magic[6]`；`static_assert(sizeof==512)`
- ☐ `octal_to_uint(s,len)`：ustar size 字段为八进制 ASCII 转 uint64
- ☐ `ramdisk_mount(void* base)`：遍历 ustar 条目（512 字节对齐），typeflag='0' 为文件，'5' 为目录，magic=`"ustar"` 验证
- ☐ CMake：将 initrd 归档嵌入内核镜像，通过 `_binary_initrd_start/end` 访问

---

### `023_fs_vfs`
**效果**：`open/read/write/close` syscall 框架可用

- ☐ `kernel/fs/vfs.hpp`：`struct Inode {ino,size,type,*fs_private, Ops{read,write,readdir}}`；`struct File {*inode,offset,flags}`；`struct FDTable {*fds[256], alloc(), close(fd)}`
- ☐ `template<T> concept FileSystemImpl` 约束 `lookup(path)→Inode*` 和 `mount(path)→bool`
- ☐ 挂载点表：`MountPoint {path[256], *fs}` 数组
- ☐ 新增 syscall：`SYS_open=2`，`SYS_close=3`；`sys_open` 查挂载点→`lookup`→分配 fd；`sys_read/write` 通过 fd→`File→Inode→Ops`

---

### `024_fs_ext2`
**效果**：挂载 QEMU ext2 分区，shell 中 `ls /` 和 `cat /etc/motd` 可用

- ☐ `Ext2Superblock [[gnu::packed]]`：`s_inodes_count/s_blocks_count/s_log_block_size/s_magic=0xEF53` 等关键字段
- ☐ `Ext2Inode [[gnu::packed]]`：`i_mode/i_uid/i_size/i_block[15]`（0-11 直接块，12 单重间接，13 双重间接）
- ☐ `Ext2DirEntry [[gnu::packed]]`：`inode/rec_len/name_len/file_type/name[]`
- ☐ `ext2_init()`：读 superblock（磁盘偏移 1024B），验证 magic，计算 block_size=`1024<<s_log_block_size`，读 block group descriptor table
- ☐ `ext2_read_inode(ino)`：group=(ino-1)/inodes_per_group，table_block=bg_inode_table，偏移=(ino-1)%inodes_per_group × inode_size
- ☐ `ext2_read_file(inode,buf,offset,len)`：遍历 `i_block[0-11]`（直接），支持 `i_block[12]`（单重间接块），不要求实现写
- ☐ `ext2_readdir(inode,index)`：遍历目录数据块的 `Ext2DirEntry` 链表（`rec_len` 步进）
- ☐ 挂载到 VFS：实现 `FileSystem` concept，`mount("/")`；shell 新增 `ls` 和 `cat` builtin 调 `sys_open/read/close`

---

## Phase 8 · GUI（长期目标）

### `025_gui_framebuffer`
**效果**：屏幕出现渐变色矩形和 `Cinux GUI` 字样

- ☐ `kernel/drivers/canvas.hpp`：`Canvas {*front_buf, *back_buf, width, height, pitch}`；`draw_pixel`，`draw_rect`，`draw_rect_outline`，`draw_line`（Bresenham），`draw_text`，`blit(dst_x,dst_y,src,w,h)`，`flip()`（back→front memcpy），`clear(color=0)`
- ☐ 双缓冲：`back_buf` 用 `kmalloc(width*height*4)` 分配，所有绘制写 back，`flip()` 一次拷贝
- ☐ 测试：绘制 10 个随机色矩形 + 标题文字，`flip()` 后屏幕显示

---

### `026_gui_wm_basic`
**效果**：可拖动窗口，Z-order 正确

- ☐ PS/2 鼠标驱动：初始化 8042 鼠标（CMD `0xA8` 启用，`0xF4` 发给鼠标激活），IRQ12 handler 解析 3 字节包（buttons/dx/dy），维护全局 `mouse_x/y`
- ☐ `kernel/gui/window.hpp`：`Window {x,y,w,h,title[64],*canvas,visible,focused,id}`；`Event {type,union{mouse,key}}`；环形事件队列
- ☐ `class WindowManager {*windows_[64],count_,*focused_,mouse_x_,mouse_y_}`：`create/destroy/raise/composite()`，`handle_mouse(dx,dy,buttons)`，`handle_key(ascii)`
- ☐ `composite()`：从低 Z-order 到高，依次 blit 各 window canvas 到屏幕 back_buf，最后 `flip()`
- ☐ 拖动：`handle_mouse` 检测 button1 按下+移动，更新 focused window 的 x/y

---

### `027_gui_native_app`
**效果**：屏幕出现可交互的终端模拟器窗口

- ☐ `user/apps/terminal.cpp`：`Terminal extends Window`；`screen_[80][25]` 字符缓冲，`cursor_x_/y_`，关联 shell 进程
- ☐ `on_key(ascii)`：发给 shell 进程（通过 pipe/共享内存）
- ☐ `on_paint()`：遍历 `screen_` 调 `canvas->draw_text`，光标用反色块
- ☐ `shell` 输出重定向到 `Terminal::write(str,len)`，更新 `screen_` 并 `on_paint()`
- ☐ WM 注册 terminal 窗口，标题栏 `Cinux Terminal`，最小化/关闭按钮占位

---

## 附录 · AI Checkpoint 工作流

```
☑ 所有 checkbox 完成
  → git tag 编号_大主题_小阶段
  → 复制本 milestone 块 → prompts/03_code_review.md {{code_snippet}}
  → 复制本 milestone 块 → prompts/04_test_generation.md {{interface_snippet}}
  → 测试全绿
  → 复制本 milestone 块 → prompts/01_tutorial_hands_on.md
  → 复制本 milestone 块 → prompts/02_tutorial_readthrough.md（附完整代码）
  → 更新本文件 ☐→☑，更新 README.md 进度表
```

### 占位符速填

| 占位符 | 来源 |
|--------|------|
| `{{current_tag}}` | 刚打的 git tag |
| `{{prev_tag}}` | `git tag` 列表倒数第二 |
| `{{phase_title}}` | milestone `###` 标题 |
| `{{milestone_goal}}` | 本节「效果」一行 |
| `{{key_files}}` | 本节所有「涉及文件」 |
| `{{checklist_items}}` | 本节所有 `☐` 条目 |
| `{{code_snippet}}` | 你写完的实际代码 |