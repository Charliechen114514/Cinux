# 004 通读版 · 完整加载小内核（下）—— 构建系统改造与设计决策

> 前置：[004B（中）分块磁盘读取完整逻辑](004-boot-load-mini-kernel-b-2.md)
> 接续：[004C 从 Bootloader 到内核的交接](004-boot-load-mini-kernel-c-1.md)

---

    }
}
```

物理起始地址 0x20000 是本章所有改动的锚点——加载地址必须和链接地址完全一致。如果链接脚本写 0x20000 但 bootloader 把数据加载到了 0x30000，所有绝对地址引用（函数调用、全局变量访问）全部指向错误的内存位置。BSS 段添加了 `__bss_start` 和 `__bss_end` 符号，供 boot.S 的 BSS 清零逻辑使用。`/DISCARD/` 段扔掉了编译器自动生成的注释、note 和异常帧信息——这些在裸机环境里没有用处，留着只会增加二进制体积。

输出格式仍然是 `elf64-x86-64`——这是编译器输出的格式，然后通过 objcopy 转成 flat binary。链接器本身需要一个合法的 ELF 格式来工作（它需要解析符号、处理重定位），只是最终产物被 strip 成了纯二进制。

### 构建系统（kernel/mini/CMakeLists.txt + cmake/qemu.cmake）

构建系统的变更主要在三处：小内核的 CMakeLists 加了 boot.S 和 objcopy 后处理，boot/CMakeLists.txt 移除了 ELF loader 相关注释，qemu.cmake 改用 mini_kernel.bin。

`kernel/mini/CMakeLists.txt`：

```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    main.cpp
)

set_target_properties(mini_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel"
)

target_compile_options(mini_kernel PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)

target_link_options(mini_kernel PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)

add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)
```

小内核现在编译两个源文件：`arch/x86_64/boot.S`（汇编入口）和 `main.cpp`（C++ 主函数）。编译选项里有几个值得关注的：`-mcmodel=large` 允许内核使用任意地址（不只是低 2GB），这对 higher-half 内核是必要的；`-mno-red-zone` 禁用 System V AMD64 ABI 的红区（128 字节的栈下溢保护区），因为内核代码随时可能被中断打断，红区里暂存的数据会被覆盖；`-fno-pie` 和 `-no-pie` 禁用位置无关可执行文件——我们的内核有固定加载地址，不需要 PIC 重定位。

POST_BUILD 步骤用 `objcopy -O binary` 把 ELF 转换成 flat binary。这一步非常关键：`-O binary` 剥离所有 ELF header、section header、符号表，只保留段的原始二进制内容。输出文件的第一个字节就是 `.text` 段的第一个字节，也就是 `_start` 的第一条指令。`$<TARGET_FILE:mini_kernel>` 是 CMake 的 generator expression，在构建时展开为实际的 ELF 文件路径。

`cmake/qemu.cmake` 中镜像构建改为使用 flat binary：

```cmake
set(MINI_BIN   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel.bin")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
)
```

注意变量名从 004A 的 `MINI_ELF` 改成了 `MINI_BIN`，路径也指向 `mini_kernel.bin` 而不是 `mini_kernel`（ELF 可执行文件）。`DEPENDS` 里的 `mini_kernel` 目标会先执行 POST_BUILD 步骤生成 .bin 文件，然后 `build_image.sh` 才会被调用。

`boot/CMakeLists.txt` 里有一段注释标记了 ELF loader 的移除：

```cmake
# ==============================================================
# C ELF Loader (32-bit freestanding)
# ==============================================================
# REMOVED: No longer needed, using flat binary format
```

原始计划中 004B 应该包含一个 32 位 C 语言编写的 ELF loader（`boot/elf_loader.c`），在 Protected Mode 下解析 ELF header、遍历 PT_LOAD 段、加载内核。这个方案在切换到 flat binary 后就不再需要了——少了几百行 C 代码，也少了一个 `-m32` 编译的 C 文件被链接进 16 位 Stage2 的麻烦。

### 构建脚本（scripts/build_image.sh）

`build_image.sh` 负责把所有组件按正确布局写入磁盘镜像。004B 的关键变更是使用 flat binary 替代 ELF 文件，并加上了大小限制检查。

```bash
MBR_BIN=${1:-${BUILD_DIR}/boot/mbr.bin}
STAGE2_BIN=${2:-${BUILD_DIR}/boot/stage2.bin}
MINI_BIN=${3:-${BUILD_DIR}/kernel/mini/mini_kernel.bin}
OUTPUT_IMAGE=${4:-${BUILD_DIR}/cinux.img}
```

三个输入文件和输出镜像路径。`MINI_BIN` 默认指向 `mini_kernel.bin`——注意不再是 ELF 文件。

磁盘布局常量：

```bash
STAGE2_LBA=1
STAGE2_MAX_SECTORS=15
MINI_KERNEL_LBA=16

MINI_KERNEL_MAX_BYTES=$((416 * 1024))    # 425984 bytes
MINI_KERNEL_MAX_SECTORS=$((MINI_KERNEL_MAX_BYTES / 512))  # 832 sectors
```

扇区 0 给 MBR，扇区 1-15 给 Stage2（最大 15 个扇区 = 7680 字节），扇区 16 开始给小内核。小内核的大小上限是 416KB（832 扇区），这个数字不是随便定的——它来自内存布局的硬约束。

脚本里有详细的大小限制验证逻辑：

```bash
if [ $MINI_SIZE -gt $MINI_KERNEL_MAX_BYTES ]; then
    log_error "Mini kernel too large!"
    log_error "       Actual:   $MINI_SIZE bytes ($MINI_SECTORS sectors)"
    log_error "       Maximum:  $MINI_KERNEL_MAX_BYTES bytes ($MINI_KERNEL_MAX_SECTORS sectors, 416KB)"
    # ... error context ...
    exit 1
fi
```

如果小内核超过 416KB，构建直接失败并打印详细的错误信息和内存布局约束说明。这种 fail-fast 的设计比运行时才发现数据写到了栈区要好得多——至少你不用对着一个无意义的 triple fault 发呆。

磁盘镜像的组装：

```bash
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
dd if="$MINI_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$MINI_KERNEL_LBA conv=notrunc status=none
```

先创建 1MB 的空白镜像，然后依次写入 MBR（扇区 0）、Stage2（从扇区 1 开始）、小内核（从扇区 16 开始）。`conv=notrunc` 确保 dd 不会截断输出文件——如果没有这个选项，写入 Stage2 时会把后面已经写好的数据全部删掉。`seek` 参数指定从哪个扇区开始写入，这就是为什么 Stage2 和小内核能正确地落在各自的位置上。

## 设计决策

### Flat Binary vs ELF 加载

**问题**：bootloader 应该加载 ELF 格式的内核还是 flat binary？

**本项目的做法**：使用 `objcopy -O binary` 把 ELF 转成 flat binary，bootloader 直接把二进制数据读到目标地址，不做任何格式解析。

**备选方案**：在 Protected Mode 下实现 ELF64 loader，解析 Program Header，遍历 PT_LOAD 段加载到各自的目标地址，处理 BSS 清零。

**为什么不选备选方案**：Real Mode 的 16 位环境做不了 64 位算术——ELF64 里的地址字段都是 64 位的，你需要在 16 位实模式下用多个寄存器拼接才能读写。虽然可以在 Protected Mode（32 位）下做 ELF 解析，但那意味着 Stage2 里要链接一个 32 位 C 编译的 ELF loader，增加了构建系统的复杂度。而且 004A 原来设计的两阶段方案（先读 header 再读完整内核）需要额外的扇区读取逻辑和地址计算，flat binary 方案把这些全部省掉了。

**如果要改进**：小内核加载大内核的时候（tag 008），mini kernel 已经有完整的 C++ 运行环境和内存管理，这时候用 ELF 格式加载大内核就是正确的选择了——可以处理任意数量的 PT_LOAD 段，做 BSS 清零，甚至将来加 ASLR 也很方便。bootloader 到 mini kernel 用 flat binary 是"简单"，mini kernel 到 big kernel 用 ELF 是"正确"，两者各取所长。

### Real Mode 完成全部磁盘读取

**问题**：磁盘读取应该在 Real Mode 一次性完成，还是部分放到 Protected Mode？

**本项目的做法**：全部在 Real Mode 完成，832 扇区一口气读完。

**备选方案**：Real Mode 只读 ELF header，Protected Mode 下用 IDE PIO 端口 I/O 自己实现磁盘驱动来读剩余部分（类似 xv6 的方案）。

**为什么不选备选方案**：在 Protected Mode 下自实现磁盘驱动意味着你需要了解 ATA PIO 协议、处理 BUSY/DRQ 状态位、等待中断或者轮询——这些代码量不比 Real Mode 的 INT 13h 调用少，而且调试难度更高（端口 I/O 出错了没有错误码，只能靠现象推断）。Real Mode 下有 BIOS 帮你处理所有硬件细节，你只需要填好 DAP 调一次中断。416KB 的读取确实需要 ~7 次循环（每次 127 扇区），但循环逻辑本身并不复杂。

### 固定地址 0x20000 vs 可配置加载地址

**问题**：小内核的加载地址应该是硬编码的固定值，还是通过某种协商机制动态决定？

**本项目的做法**：硬编码 0x20000，在链接脚本、boot.S 常量、build_image.sh 中保持一致。

**备选方案**：在磁盘镜像的固定偏移处放一个 header，包含加载地址和大小信息，bootloader 读取后动态加载。

**为什么不选备选方案**：动态发现机制在内核开发初期是过度设计。固定地址的调试优势非常明显——你知道小内核永远在 0x20000，可以直接用 GDB 的 `x/10i 0x20000` 查看前 10 条指令。而且 0x20000 这个地址不是拍脑袋定的，它来自对低 1MB 内存布局的仔细分析：避开 Real Mode 栈（0x9000~0x19000），留足安全间隙（28KB），同时不超过 Protected Mode 栈（0x90000）减去 32KB gap 的上限（0x88000），可用空间正好是 0x88000 - 0x20000 = 0x68000 = 416KB。

**如果要改进**：如果将来小内核膨胀到超过 416KB，有两个选择——要么把 Protected Mode 栈挪到更高的地址（比如 0x100000），要么在进入 Long Mode 后使用 64 位磁盘驱动把内核加载到更高的物理地址。两种方案都意味着更多的代码变更，但至少当前的固定地址设计把约束条件列得清清楚楚，改起来不懵。

## 扩展方向

1. **串口输出和 E820 打印**（难度：简单）——在 mini kernel 里加入串口驱动（tag 005 的内容），把 BootInfo 里的内存映射条目打印出来。你会看到 QEMU 的 6~7 条 E820 记录，包括低 640KB 可用 RAM、ACPI 数据区、高内存区域等。

2. **多引导协议支持**（难度：中等）——当前 BootInfo 是 Cinux 私有格式，可以扩展为兼容 Multiboot2 规议的 tag 链式结构，这样就能用 GRUB 直接引导 Cinux，省掉自写 bootloader 的麻烦。

3. **BootInfo 校验和**（难度：简单）——给 BootInfo 加一个 checksum 或 magic number 字段，内核入口处验证数据完整性。这个改动很小，但能帮你及早发现 bootloader 没有正确填充 BootInfo 的 bug。

4. **动态内核大小探测**（难度：中等）——当前内核大小（832 扇区）是编译时固定的。可以在磁盘镜像的固定位置（比如扇区 15 的最后几个字节）写入内核的实际扇区数，bootloader 读取后动态确定读取量，这样小内核大小变化时不需要同步修改 boot.S 的常量。

5. **真实硬件测试**（难度：困难）——把 Cinux 写到 U 盘上在真实 PC 上启动。需要处理的主要差异：BIOS 的 INT 13h 实现行为可能不同（有些 BIOS 的单次读取上限不到 127）、A20 gate 需要额外的启用方式、USB 启动可能需要 USB emulation 模式支持。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。Real Mode、Protected Mode、Long Mode 的状态转换和寄存器行为。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=42h 扩展读取的完整规范。单次调用上限、DAP 16 字节结构、DS:SI 指向 DAP 的要求。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — Rolling Your Own Bootloader: 自写 bootloader 的综合指南。关于选择加载地址（避开栈区和 BIOS 数据区）、flat binary vs ELF 的取舍讨论。
  https://wiki.osdev.org/Bootloader

- OSDev Wiki — Multiboot: Multiboot/Multiboot2 规范定义的 bootloader-kernel 交接设计。BootInfo 的设计借鉴了其思路。
  https://wiki.osdev.org/Multiboot

- Linux x86 Boot Protocol: Linux 内核的 bootloader-kernel 交接协议。`struct boot_params` 的设计理念和 Cinux 的 BootInfo 相同——固定地址 + 结构体 + 寄存器传递指针。
  https://www.kernel.org/doc/html/latest/arch/x86/boot.html

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的实现，与 Cinux 的 Real Mode + BIOS INT 13h 方案形成对比。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
