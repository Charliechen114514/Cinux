# 004-C 动手篇（补完）：构建系统、debugcon 配置与端到端验证

> 本章完成后的可见效果：`debug.log` 输出完整序列 `OPLJ123G4===CPPGC1V123B===END`
>
> 前置要求：已完成本系列上篇（BootInfo 填充与高半核跳转）和中篇（内核入口与 C++ 运行时）

---

## 导语

前两篇我们把 BootInfo 填充、高半核跳转、内核入口序列、C++ 运行时支撑这些核心功能都讲完了。但还差几个基础设施层面的事情没做：debugcon 需要在正常模式下也能输出（不然 `make run` 什么都看不到）、Mini Kernel 的 CMakeLists.txt 需要配置正确的编译选项和链接脚本、objcopy 后处理步骤需要确保 flat binary 正确生成。这些事情单个来看都不复杂，但漏了任何一个都会导致构建失败或者运行时看不到输出。

这一篇就专门把这些收尾工作讲清楚，然后做一次完整的端到端验证——从 CMake 配置到 QEMU 启动，确认 `OPLJ123G4===CPPGC1V123B===END` 这个字符序列完整出现。

---

## 概念精讲

### objcopy -O binary：从 ELF 到 Flat Binary 的转换

我们的内核编译链的最终产物是 flat binary，但编译和链接阶段产出的是 ELF 格式。这两者的区别在于：ELF 包含丰富的元数据（section header table、program header table、符号表、重定位信息），链接器需要这些信息来解析符号和计算地址。但 Bootloader 不会解析 ELF——它只是用 INT 13h 把磁盘上的二进制数据原封不动地读到物理地址 0x20000，然后跳转过去。

`objcopy -O binary` 做的事情就是把 ELF 转成 Bootloader 能用的格式：剥离所有 ELF 元数据，只保留各段的原始二进制内容，按地址顺序排列输出。输出文件的起始地址是 ELF 中最低的 LMA（Load Memory Address），在我们的链接脚本中就是 0x20000。但由于 flat binary 没有"基地址"的概念，objcopy 实际上从 LMA=0 开始输出——也就是说，文件开头对应的是 LMA 最低的那个段的第一个字节。

这个转换过程有一个容易忽视的细节：`.bss` 段不会出现在 flat binary 中。BSS 在 ELF 里不占空间（只记录大小和起始地址），`objcopy -O binary` 会跳过它。这是正确的行为——BSS 段的内容应该全是零，由运行时（`boot.S` 中的 `rep stosb`）负责清零，不需要在文件中占空间。如果 BSS 被包含进了 flat binary，内核的磁盘镜像会无故增大，而且 Bootloader 加载的那段零值数据在 BSS 清除时又会被重新清零一次，完全是浪费。

### -mcmodel=large：为什么不能用默认的 small 模型

x86-64 的代码模型决定了编译器如何生成访问全局变量和函数的指令。默认的 `small` 模型假设所有代码和数据都在低 2GB 地址空间内（0 到 0x7FFFFFFF），编译器可以用 32 位相对地址或绝对地址来引用符号——这在用户态程序中完全没问题，因为虚拟地址空间通常从 0x400000 附近开始。

但我们的内核 VMA 是 `0xFFFFFFFF80020000`，这个地址远超 2GB。如果用 small 模型，编译器生成类似 `movq symbol(%rip), %rax` 这样的 RIP-relative 寻址指令时，32 位的偏移量不够表示从当前指令位置到 `0xFFFFFFFF80020000` 的距离——溢出后变成一个完全错误的地址，运行时访问到垃圾数据或者直接 Page Fault。

`-mcmodel=large` 告诉编译器使用 64 位绝对地址来引用符号——每条全局变量访问都需要 `movabsq $0xFFFFFFFF80022XXX, %rax` 这样的 10 字节指令（REX 前缀 + 操作码 + 8 字节立即数）。代码体积会增大（每条全局变量访问多几个字节），但对于内核代码来说这点开销完全可以接受。

### -mno-red-zone：内核代码的安全红线

x86-64 ABI 定义了一个叫 "Red Zone" 的优化区域：栈指针（%rsp）以下 128 字节的空间被保留给当前函数使用，信号处理程序和中断处理程序不能触碰这个区域。在用户态程序中，这 128 字节可以用来存储局部变量而不需要调整 %rsp，省掉了 push/pop 的开销。

但在内核代码中，这个优化是致命的。内核代码随时可能被硬件中断打断——中断发生时 CPU 不会帮你保存 Red Zone 的内容（Red Zone 的设计假设中断处理会切换到内核栈，但在内核本身就是中断处理者的情况下这个假设不成立）。如果编译器把某个关键变量存在了 Red Zone 里，中断到来时这个变量就会被中断处理程序的栈帧覆盖，导致数据损坏和难以调试的随机崩溃。

`-mno-red-zone` 禁用这个优化，强制编译器为所有局部变量显式分配栈空间。代价是每帧多几字节的栈使用和一两条额外的 `sub %rsp` 指令，但避免了中断上下文中的数据损坏。

---

## 动手实现

### Step 1: 更新 cmake/qemu.cmake——debugcon 全局启用

**目标**：把 debugcon 相关的 QEMU 参数从 `QEMU_DEBUG_FLAGS` 移到 `QEMU_COMMON_FLAGS`，确保 `make run`（非调试模式）也能看到 debugcon 输出。

**设计思路**：之前 debugcon 只在 `make run-debug` 模式下启用，正常模式不启用。但从 004C 开始，内核的 `boot.S` 和 `main.cpp` 都依赖 debugcon 输出来验证执行状态——如果正常模式下不启用，`outb %al, $0xE9` 就是一个空操作，你什么调试信息都看不到。把它移到 COMMON 后，不管以什么模式运行，debugcon 输出都会写入 `debug.log` 文件。

**实现约束**：修改位置在 `cmake/qemu.cmake` 文件。`QEMU_COMMON_FLAGS` 需要包含 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9` 这两个参数。`QEMU_DEBUG_FLAGS` 只保留 `-s -S`（GDB 调试用的参数）。不要删掉其他已有的 COMMON 参数（`-m 512M -serial stdio -no-reboot -no-shutdown`）。

**验证**：运行 `make run` 后检查 `debug.log` 文件是否存在且内容不为空。如果文件不存在，检查 CMake 配置是否正确应用了新的 QEMU 参数。可以用 `cmake --build build --target help` 确认 build 目标存在，或者直接 `./build/kernel/mini/mini_kernel --help` 不行的话就手动检查 `build/compile_commands.json` 里 QEMU 参数是否正确。

**踩坑预警**：`-global isa-debugcon.iobase=0xe9` 这个参数看起来多余（QEMU 的 isa-debugcon 默认端口就是 0xE9），但显式指定是为了防止未来的默认值变更。如果你省略了这个参数，在某次 QEMU 升级后可能突然发现 debugcon 不工作了，排查起来非常隐蔽。另外一个容易搞错的地方是 `-debugcon file:debug.log`——注意是 `file:` 前缀，不是 `stdio:` 或 `vc:`。`file:` 模式把输出写到文件，不影响 QEMU 的标准输出（标准输出已经被 `-serial stdio` 占用了）。

### Step 2: 配置 Mini Kernel 的 CMakeLists.txt

**目标**：确保 `kernel/mini/CMakeLists.txt` 配置了正确的源文件、编译选项、链接选项和后处理步骤。

**设计思路**：源文件列表需要包含三个核心文件：`arch/x86_64/boot.S`（内核入口汇编）、`arch/x86_64/crt_stub.cpp`（C++ 运行时支撑）、`main.cpp`（内核主函数）。CMake 会自动识别 `.S` 文件需要用 GCC 的汇编模式编译（带 C 预处理），`.cpp` 文件用 C++ 编译。

编译选项的关键几项：`-ffreestanding`（不依赖标准库）、`-fno-exceptions`（禁用异常）、`-fno-rtti`（禁用 RTTI）、`-fno-pie`（不做位置无关可执行文件）、`-mcmodel=large`（大代码模型，允许访问任意 64 位地址——高半核地址远超默认 small 模型的 2GB 限制）、`-mno-red-zone`（禁用 red zone——内核代码不能依赖 red zone，因为中断可能在任何时刻打断执行，而中断处理不会帮你保存 red zone 的内容）。

链接选项需要 `-T linker.ld` 指定链接脚本、`-nostdlib` 不链接标准库、`-no-pie` 不做位置无关。Post-build 步骤用 `objcopy -O binary` 把 ELF 转成纯二进制（`mini_kernel.bin`），因为 Bootloader 不知道怎么解析 ELF 格式——它只是把磁盘上的二进制数据原样读入内存然后跳转。

**踩坑预警**：`-mcmodel=large` 是不能省的。如果用了默认的 `-mcmodel=small`，编译器生成的代码会假设所有符号都在低 2GB 地址空间内，用 32 位相对地址来引用全局变量。但我们的内核 VMA 是 0xFFFFFFFF80000000，远超 2GB——运行时这些地址引用会被截断成错误的值。这个坑的特点是编译和链接都不会报错，只有运行时才炸，排查难度极高。

include 路径需要包含 `boot/` 目录（项目根目录下的），因为 `main.cpp` 要引用 `boot_info.h`。

**验证**：编译后确认 `build/kernel/mini/mini_kernel.bin` 文件存在且大小合理（应该在几十 KB 左右）。用 `file build/kernel/mini/mini_kernel` 确认它是 ELF 64-bit x86-64 格式，用 `readelf -h build/kernel/mini/mini_kernel` 确认入口地址是 `0xffffffff80020000`。如果入口地址不对，说明链接脚本没有正确加载。

**额外检查**：用 `readelf -S build/kernel/mini/mini_kernel | grep -E "\.text|\.data|\.init_array|\.bss"` 确认所有段都存在。特别是 `.init_array` 段——如果它不存在，说明链接器的 section garbage collection 把它砍掉了（`KEEP()` 没写对），全局构造函数永远不会被调用。用 `objdump -t build/kernel/mini/mini_kernel | grep boot_info` 确认 `__boot_info_ptr` 在 `.data` 段而不是 `.bss` 段。

### Step 3: 完整的端到端验证

**目标**：从头构建整个项目，运行 QEMU，确认 debugcon 输出完整序列。

**验证步骤**：

先确认所有源文件都已正确保存，然后执行以下步骤：

1. 清空 build 目录重新构建（排除缓存问题）
2. 运行 CMake 配置和编译
3. 运行 QEMU
4. 检查 `debug.log` 文件内容
5. 如果有任何字符缺失，按步骤 4 的排查手册定位问题

预期的完整输出序列是 `OPLJ123G4===CPPGC1V123B===END`。逐字符解读：

- `O`：MBR 加载 Stage2 成功
- `P`：保护模式切换完成
- `L`：Long Mode 进入
- `J`：Bootloader 跳转到 Mini Kernel
- `1`：内核入口 `_start` 到达
- `2`：栈设置完成
- `3`：BSS 清除完成
- `G`：全局对象 `GlobalCounter` 构造（在 `_init_global_ctors` 中）
- `4`：全局构造函数调用完毕
- `===CPP`：C++ 特性测试开始
- `C1`：简单类构造验证
- `V`：虚函数类构造
- `2`：虚函数测试通过
- `3`：全局对象验证通过
- `B`：BootInfo 验证通过
- `===END`：全部测试通过

如果序列在某处中断，根据最后一个字符就能定位问题。比如停在 `OPLJ12` 说明 BSS 清除出了问题，停在 `123G` 说明全局构造之后的恢复出了问题，停在 `===CPPC1V` 说明虚函数之后的某个验证失败了。

### Step 4: 异常排查手册

如果端到端验证没有通过，按下面的流程排查：

**问题：只看到 OPL，没有 J**——BootInfo 填充或跳转代码没有执行。检查 `stage2.S` 中 `long_mode_entry` 之后的代码。

**问题：看到 J 但立刻 Triple Fault**——高半核页表映射有问题。用 GDB 检查 PML4[511] 和 PDPT[510] 的内容。

**问题：看到 J1 但没有 2**——栈设置失败。检查 `__mini_stack_top` 符号是否存在（链接脚本中的 `.bss` 段末尾）。

**问题：看到 12 但没有 3**——BSS 清除出了问题。检查 `__bss_start` 和 `__bss_end` 的值是否正确（链接脚本中的符号）。

**问题：看到 123 但没有 G4**——全局构造函数调用失败。检查 `.init_array` 段是否存在（用 `readelf -S build/kernel/mini/mini_kernel | grep init_array`），检查 `__init_array_start` 和 `__init_array_end` 是否正确。

**问题：看到 123G4 但没有 ===CPP**——`mini_kernel_main` 没有被调用，或者 `__boot_info_ptr` 恢复失败。检查 boot.S 最后 `call mini_kernel_main` 之前的 `movq __boot_info_ptr, %rdi` 是否正确。也可能是 `mini_kernel_main` 函数本身有编译或链接错误——用 `objdump -t build/kernel/mini/mini_kernel | grep mini_kernel_main` 确认符号存在。如果符号不存在，检查 `main.cpp` 是否有 `extern "C"` 标记——没有这个标记的话 C++ 编译器会对函数名做 name mangling，汇编里的 `call mini_kernel_main` 就找不到对应的符号。

**问题：看到 ===CPP 但后续字符不完整**——某个 C++ 特性验证失败了。根据最后一个输出的字符定位是哪个测试出了问题。比如 `C1` 之后没有 `V`，说明虚函数类的构造函数出了问题；有 `V` 但没有 `2`，说明虚函数调用本身失败（可能是 vtable 损坏或 `__cxa_pure_virtual` 被调用）；有 `2` 但没有 `3`，说明全局对象的构造函数没有被调用（`.init_array` 段可能被链接器丢弃了）。

**问题：QEMU 启动后直接重启，连 O 都没有**——MBR 加载就失败了。检查 `build_image.sh` 是否正确把 MBR 写入了磁盘镜像的 LBA 0。也可能是磁盘镜像本身损坏了——删除后重新生成。

**问题：看到 OPLJ123G4===CPPGC1V123B===END 后 QEMU 一直在循环重启**——`mini_kernel_main` 的末尾 `while(1) { cli; hlt }` 循环没有正确执行。可能是编译器优化把死循环优化掉了（加 `-O0` 或者用 `__asm__ volatile` 确保不被优化），也可能是 `hlt` 指令之后的 `cli` 没有紧跟（检查生成的汇编）。

### Step 5: 用 objdump 和 readelf 深入检查构建产物

当端到端验证失败时，以下命令可以帮助快速定位问题：

检查 ELF 入口地址：
```bash
readelf -h build/kernel/mini/mini_kernel | grep "Entry point"
```
应该输出 `Entry point address: 0xffffffff80020000`。

检查段的 VMA 和 LMA：
```bash
readelf -l build/kernel/mini/mini_kernel
```
注意看 `VirtAddr` 和 `PhysAddr` 列——`.text` 段的 VirtAddr 应该从 `0xffffffff80020000` 开始，PhysAddr 应该从 `0x20000` 开始。

检查符号表中的关键符号：
```bash
objdump -t build/kernel/mini/mini_kernel | grep -E "boot_info|init_array|bss_start|bss_end|_start|mini_kernel"
```
确认每个符号都在预期的段中，且地址没有重叠。

检查 flat binary 大小：
```bash
ls -la build/kernel/mini/mini_kernel.bin
```
大小应该在几十 KB 左右。如果太小（比如只有几百字节），说明 objcopy 命令参数有误；如果太大（超过 416KB），说明 BSS 段被包含进了 flat binary（不应该发生，因为 objcopy -O binary 会跳过 BSS）。

### Step 6: 用 GDB 验证完整启动序列

如果上面的排查步骤没有解决问题，可以用 GDB 进行更深度的调试。推荐的调试流程：

1. 用 `make run-debug` 启动 QEMU（会等待 GDB 连接）
2. 在另一个终端启动 GDB：`gdb build/kernel/mini/mini_kernel`
3. 在 GDB 中连接 QEMU：`target remote :1234`
4. 设置断点：`break _start` 和 `break mini_kernel_main`
5. `continue` 运行到 `_start` 断点
6. 检查 `%rdi`：`info registers rdi` 应该显示 `0x7000`
7. 单步执行到 `rep stosb` 之后：检查 `%rdi` 已经改变（不再是 0x7000）
8. 检查 `__boot_info_ptr` 的值：`x/gx &__boot_info_ptr` 应该显示 `0x7000`
9. `continue` 到 `mini_kernel_main` 断点
10. 检查 BootInfo 内容：`x/gx (uint64_t*)__boot_info_ptr` 应该显示 `0xFFFFFFFF80020000`

这个调试流程可以帮助你精确定位问题出现在哪个阶段——是 BootLoader 传递参数出错，还是 BSS 清除破坏了保存的指针，还是全局构造函数覆盖了 `.data` 段的数据。每一步的 GDB 输出都应该和预期值一致，如果不一致就找到了问题所在。

还有一个非常有用的 GDB 技巧：在 QEMU 中可以用 `-d int` 选项记录所有中断和异常。如果你看到 Triple Fault，这个日志会告诉你是哪种异常触发的（Page Fault、General Protection Fault、Double Fault 等），以及异常发生时的 `%rip` 值——有了 `%rip` 就知道是哪条指令出了问题。

## Tag 边界确认

最后确认一下 004C 三个文件各自的覆盖范围。c-1（上篇）覆盖 Bootloader 侧：高半核页表映射、BootInfo 填充、跳转到内核。c-2（中篇）覆盖内核侧：SSE 启用、栈设置、BSS 清除与指针保存、符号冲突修复、C++ 运行时（crt_stub）、链接器脚本。c-3（本篇）覆盖构建系统：CMakeLists.txt 编译/链接选项、debugcon 配置、objcopy 后处理、端到端验证、异常排查。三个文件合起来覆盖了 004C tag 的全部内容，和 004A（Real Mode E820+磁盘读取）和 004B（BootInfo 定义+416KB 加载+构建系统）的边界清晰，没有重叠。特别注意 004B 和 004C 的分界线：004B 负责把完整的 Mini Kernel flat binary 从磁盘读到物理地址 0x20000，004C 从 Long Mode 进入后开始工作（高半核映射、BootInfo 填充、跳转）。004B 的最后一件事是调用 `enter_long_mode` 进入 Long Mode 并输出 `L`，004C 的第一件事就是在 Long Mode 中填充 BootInfo。这个分界线非常清晰——`L` 字符的输出属于 003/004B 的阶段，`J` 字符的输出属于 004C。

## 本章小结

| 概念 | 要点 |
|------|------|
| debugcon 全局启用 | `-debugcon file:debug.log` 移到 COMMON_FLAGS，确保所有运行模式可见 |
| 编译选项 | `-ffreestanding -fno-exceptions -fno-rtti -mcmodel=large -mno-red-zone` |
| `-mcmodel=large` | 高半核地址超出 small 模型 2GB 限制，必须用 large 模型访问任意 64 位地址 |
| `-mno-red-zone` | 内核代码不能依赖 red zone，中断随时打断会覆盖 red zone 数据 |
| 链接选项 | `-T linker.ld -nostdlib -no-pie` |
| objcopy 后处理 | `-O binary` 将 ELF 转为 flat binary，Bootloader 直接加载，BSS 不包含在输出中 |
| ELF 入口地址 | `readelf -h` 确认入口为 `0xffffffff80020000` |
| 段检查 | `.init_array` 必须存在（`KEEP()` 保护），`__boot_info_ptr` 必须在 `.data` 段 |
| 端到端验证序列 | `OPLJ123G4===CPPGC1V123B===END`，每个字符对应一个检查点 |
| 构建产物 | `build/kernel/mini/mini_kernel`（ELF）+ `mini_kernel.bin`（flat binary） |
| GDB 调试流程 | `break _start` -> 检查 `%rdi`=0x7000 -> 单步到 BSS 清除后 -> 检查 `__boot_info_ptr` |
| 004C 三篇分工 | c-1=Bootloader 侧(高半核+BootInfo+跳转), c-2=内核侧(SSE+栈+BSS+C++ 运行时), c-3=构建系统(编译选项+debugcon+验证) |
