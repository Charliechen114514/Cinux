---
title: 001-boot-real-mode-3 · 实模式引导
---

# 001 Read-through (3/3) —— 构建系统：从 .S 到磁盘镜像的完整链路

## 概览

本文讲解 `boot/CMakeLists.txt`、`cmake/qemu.cmake` 和 `scripts/build_image.sh`。这三个文件共同构成了从汇编源码到可启动磁盘镜像的完整构建链路。CMake 负责编译和链接（.S → .o → ELF），objcopy 负责格式转换（ELF → raw binary），build_image.sh 负责组装磁盘镜像（MBR + Stage2 → .img）。

理解构建系统对 bootloader 开发至关重要——很多看起来像代码 bug 的问题（链接地址不对、二进制尺寸异常、objcopy 后数据丢失）其实都是构建配置的问题。这一篇会详细解释每一步为什么这样做。

## 架构图

```
构建流程：

  mbr.S ──► [as --32] ──► mbr.o ──► [ld elf_i386, mbr.ld] ──► mbr (ELF)
                                                                       │
                                                            [objcopy -O binary]
                                                                       │
                                                                   mbr.bin (512B)
                                                                       │
  stage2.S ─► [as --32] ─► stage2.o ─┐                                 │
                                      ├──► [ld elf_i386, stage2.ld] ──► stage2 (ELF)
  common/serial.S ─► [as --32] ─► boot_common.o ─┘                        │
                                                              [objcopy -O binary]
                                                                       │
                                                               stage2.bin (~2KB)
                                                                       │
                                                          ┌────────────┘
                                                          │
                                                    build_image.sh
                                                          │
                                                    cinux.img (1MB)
                                                    ├─ Sector 0: MBR
                                                    └─ Sector 1+: Stage2
```

## 代码精讲

### boot_common——共享函数的目标文件库

```cmake
add_library(boot_common OBJECT
    common/serial.S
)
target_compile_options(boot_common PRIVATE
    -Wa,--32
)
```

`OBJECT` 类型的库不会生成独立的 .a 或 .so 文件，而是编译成 .o 目标文件供其他目标直接引用。这正好适合我们的场景——common/serial.S 不需要单独链接成可执行文件，它只是被 stage2 链接时拉进来。`-Wa,--32` 告诉汇编器生成 32 位目标文件——注意这不是说代码是 32 位的（代码用 `.code16gcc` 标注为 16 位），而是说目标文件格式是 32 位 ELF（ELF32），包含的指令编码在 16 位模式下仍然正确。

为什么不直接把 common/serial.S 加到 stage2 的源文件列表里？用 OBJECT 库的好处是：如果未来 MBR 也需要某些共享函数（比如一个新的轻量级 print），可以直接复用 boot_common 目标，不用重复编译。同时保持了源文件级别的依赖清晰——stage2.S 依赖 boot_common 是显式声明的。

### MBR 目标——512 字节的精确控制

```cmake
add_executable(mbr
    mbr.S
)
target_compile_options(mbr PRIVATE
    -Wa,--32
)
target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)
```

MBR 只包含 mbr.S 一个源文件——没有 boot_common。这是前一篇提到的设计决策：MBR 必须控制在 512 字节以内，链接额外的目标文件会导致代码膨胀。链接选项的每一项都有讲究：

- `-Wl,-m,elf_i386`：强制使用 32 位 ELF 链接器。在 x86_64 主机上默认链接器是 elf_x86_64，但 MBR 是 16 位实模式代码，需要 32 位格式。
- `-T mbr.ld`：指定链接脚本，设置起始地址为 0x7C00（MBR 的实际运行地址）。
- `-nostdlib`：不链接标准库和启动文件（crt0.o 等）。bootloader 是裸机代码，没有任何运行时支持。
- `-no-pie`：禁用位置无关可执行文件。PIE 会生成额外的重定位表和 .got 段，在裸机环境下无用且占空间。

### Stage2 目标——链接共享函数

```cmake
add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>
)
target_compile_options(stage2 PRIVATE
    -Wa,--32
)
target_link_options(stage2 PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
    -nostdlib
    -no-pie
)
```

`$<TARGET_OBJECTS:boot_common>` 是 CMake 的生成器表达式，在构建时展开为 boot_common 目标的 .o 文件路径。stage2.S 和 common/serial.S 的目标文件被一起链接成 stage2 可执行文件。

### 链接脚本——地址模型的根本

MBR 链接脚本：

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : {
        *(.text)
        *(.rodata)
    }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")
```

`. = 0x7C00` 设置起始地址——链接器会把所有符号地址从 0x7C00 开始计算。这样 `msg_booting` 的地址会是 0x7C00 + 代码段大小的偏移，运行时通过 DS:SI 访问时（DS=0），物理地址正好对应实际内存位置。`.rodata` 合并到 `.text` 段里确保字符串常量和代码在同一个连续区域——对 MBR 来说很重要，因为 `.org 510` 的填充逻辑假设所有内容在 `.text` 段内连续排列。

Stage2 链接脚本的关键差异：

```cmake
"    . = 0x0;"
```

起始地址是 0 而不是 0x8000。原因在 read-through 2/3 里已经解释过：MBR 用 `ljmp $0x0800, $0` 跳转，CS=0x0800 提供了段偏移（0x0800 × 16 = 0x8000），链接地址不需要再加这层偏移。如果写成 `. = 0x8000`，所有数据引用都会多算一个 0x8000——代码能执行（因为 CS:IP 计算正确），但数据访问全崩。

`/DISCARD/` 段丢弃 `.comment` 和 `.note`——这些是编译器自动生成的元数据（编译器版本号等），对裸机程序无用，丢弃可以减小二进制尺寸。

### objcopy——从 ELF 到纯二进制

```cmake
add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
)
```

链接器输出的是 ELF 格式可执行文件（包含段头、符号表等元数据），但 BIOS 不认识 ELF——它只认原始的二进制机器码。`objcopy -O binary` 把 ELF 转成纯二进制：剥离所有 ELF 头和段头，只保留 `.text`、`.data`、`.bss` 段的原始内容。输出文件就是可以被 BIOS 直接加载的 512 字节二进制。

对 MBR 来说，`objcopy` 输出恰好 512 字节——因为 `.org 510` 加上 `.word 0xAA55` 精确填充到了 512 字节末尾。如果代码超过 510 字节，objcopy 的输出会超过 512 字节，build_image.sh 不会报错（它只检查 Stage2 的大小），但 BIOS 只读第一个扇区，超出的部分不会被加载。

### build_image.sh——磁盘镜像的组装流水线

```bash
# Step 1: Create blank image (1MB = 2048 sectors)
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# Step 2: Write MBR to sector 0
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none

# Step 3: Write Stage2 starting at sector 1
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
```

三步组装：先用 `dd` 创建 1MB 的全零文件作为磁盘镜像骨架，然后写入 MBR（扇区 0），最后写入 Stage2（从扇区 1 开始）。`conv=notrunc` 很重要——不截断输出文件。如果没有这个选项，写入 Stage2 时 dd 会把文件截断到 Stage2 数据末尾，丢掉后面还没用到的零填充空间。`seek=$STAGE2_LBA` 让 dd 在输出文件中跳过 1 个扇区（512 字节），从扇区 1 开始写入 Stage2。

脚本还包含验证逻辑：

```bash
STAGE2_SECTORS=$(( (STAGE2_SIZE + 511) / 512 ))
if [ $STAGE2_SECTORS -gt $STAGE2_MAX_SECTORS ]; then
    echo "Error: Stage2 too large"
    exit 1
fi
```

Stage2 的实际大小被换算成扇区数，如果超过 15 扇区就报错——因为 MBR 的 DAP 里硬编码了读取 15 个扇区，超过的部分不会被读进来。还有 MBR 签名验证：读出镜像偏移 510-511 的两个字节，检查是否为 `55aa`。

### cmake/qemu.cmake——构建依赖的正确声明

```cmake
set(MBR_BIN    "${CMAKE_BINARY_DIR}/boot/mbr.bin")
set(STAGE2_BIN "${CMAKE_BINARY_DIR}/boot/stage2.bin")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2
)
```

`DEPENDS mbr stage2` 确保 MBR 和 Stage2 都构建完成后才执行镜像组装。如果只写 `DEPENDS mbr`，修改 stage2.S 后重新构建不会触发镜像更新——因为 CMake 认为镜像不依赖 stage2 目标。这个依赖声明还间接拉起了 objcopy 的 POST_BUILD 命令（因为 mbr 和 stage2 目标构建完会触发各自的 POST_BUILD），所以整个链路是：源码改动 → 编译 → 链接 → objcopy → build_image.sh → 新镜像。

## 设计决策

### 决策：使用 `-Wa,--32` 而非 `-m32` 编译 16 位代码

**问题**：MBR 和 Stage2 是 16 位实模式代码，但在 x86_64 主机上编译。

**本项目的做法**：用 `-Wa,--32` 只影响汇编器，生成 32 位 ELF 目标文件。

**备选方案**：用 `-m32` 让整个工具链（包括链接器）工作在 32 位模式。

**为什么不选备选方案**：`-m32` 需要安装 32 位的 GCC 和 binutils（在 Ubuntu 上是 gcc-multilib 包），增加了环境依赖。`-Wa,--32` 只影响汇编器阶段（因为我们的源文件全是 .S 汇编文件，不经过 C 编译器），链接阶段用 `-Wl,-m,elf_i386` 单独指定 32 位链接器。这种组合在只写汇编的 bootloader 场景下更轻量。等后续内核引入 C++ 代码时（tag 004+），会需要 `-m32` 或交叉编译工具链。

### 决策：链接脚本用 `file(WRITE)` 在构建目录生成

**问题**：链接脚本需要动态路径或构建时生成。

**本项目的做法**：用 CMake 的 `file(WRITE)` 在构建目录下生成 .ld 文件。

**备选方案**：把链接脚本作为独立文件放在源码目录。

**为什么不选备选方案**：两种方式都可以，但 `file(WRITE)` 的好处是链接脚本内容直接可见在 CMakeLists.txt 里——读一个文件就能看完整构建配置。对于 bootloader 这种链接脚本很短（不到 15 行）的场景，放在 CMakeLists.txt 里比单独建文件更直观。如果链接脚本变复杂了（比如需要处理多个 .text 子段），再拆出来也不迟。

## 扩展方向

- **添加 MBR 大小检查**（难度：⭐）——在 objcopy 后检查 mbr.bin 是否恰好 512 字节，超过则构建失败。
- **自动计算 Stage2 扇区数**（难度：⭐）——从 stage2.bin 的实际大小自动计算 DAP 中的扇区数，而非硬编码 15。
- **添加 `make run` 便捷目标**（难度：⭐）——在 CMake 中添加自定义目标，构建后自动启动 QEMU。

## 参考资料

- OSDev Wiki: [MBR (x86)](https://wiki.osdev.org/MBR_(x86)) — MBR 签名和格式规范
- OSDev Wiki: [Bare Bones](https://wiki.osdev.org/Bare_Bones) — 交叉编译和裸机构建基础
- GNU LD Manual: [Linker Scripts](https://sourceware.org/binutils/docs/ld/Scripts.html) — SECTIONS 命令和地址计算
