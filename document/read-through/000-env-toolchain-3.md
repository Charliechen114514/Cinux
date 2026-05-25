---
title: 000-env-toolchain-3 · 环境搭建
---

# 000-3 · QEMU 集成与镜像构建

## 概览

本文是 tag `000_env_toolchain` 的第三篇通读，聚焦两个还未详细展开的子系统：QEMU 集成配置（`cmake/qemu.cmake`）和镜像构建脚本（`scripts/build_image.sh`）。这两个文件在 read-through 第一篇里被简要提及，但它们的实现细节值得单独拿出来讲——因为它们牵涉到磁盘布局、dd 操作、QEMU 参数选择等实战知识，这些在后续 tag 里会反复用到。同时，本文也会对整个 tag 000 的设计决策做一个汇总回顾。

## 架构图

```
cmake/qemu.cmake 的 target 注册：

    make run          -> 启动 QEMU（正常运行）
    make run-debug    -> 启动 QEMU（GDB server :1234）
    make image        -> 只构建磁盘镜像

    依赖链：

    mbr.S -> mbr.bin ----+
                          |---> build_image.sh ---> cinux.img ---> QEMU
    (后续 stage2/内核) ---+

build_image.sh 的磁盘布局：

    扇区 0        : MBR (512 bytes)
    扇区 1-15     : Stage2 (最多 15 个扇区 = 7680 bytes)
    扇区 16+      : Mini kernel
    扇区 848+     : Big kernel (可选)

    +--------+--------+----------+----------+----------+
    |  MBR   | Stage2 |   gap    | Mini Krn | Big Krn  |
    | 512B   | <=7.5KB|          |  <=416KB | (可选)    |
    +--------+--------+----------+----------+----------+
    LBA 0    LBA 1               LBA 16     LBA 848
```

## 代码精讲

### QEMU 配置 — 查找与参数

```cmake
find_program(QEMU_EXECUTABLE qemu-system-x86_64)

if(NOT QEMU_EXECUTABLE)
    set(QEMU_EXECUTABLE "qemu-system-x86_64")
    message(WARNING "qemu-system-x86_64 not found in PATH, using default name")
endif()
```

`find_program` 在系统 PATH 里查找 `qemu-system-x86_64`。如果没找到，不直接报错，而是设一个默认值并打印警告——这是因为用户可能在非标准路径安装了 QEMU，或者只是暂时没加入 PATH。实际执行 `make run` 的时候如果 QEMU 真的不存在，系统自然会报 "command not found"。

```cmake
if(EXISTS "/dev/kvm")
    set(QEMU_ACCEL -accel kvm -cpu max)
endif()
```

这段检测宿主机是否支持 KVM 加速。`/dev/kvm` 是 Linux KVM 模块创建的设备节点，存在说明内核支持硬件虚拟化。有 KVM 的话加上 `-accel kvm -cpu max`，QEMU 可以直接在 CPU 上执行客户机代码，性能接近原生。CI 环境（比如 GitHub Actions）通常没有 KVM，所以这段检测是必要的。

```cmake
set(QEMU_COMMON_FLAGS
    -m ${QEMU_MEMORY}
    -serial stdio
    -no-reboot
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
    ${QEMU_ACCEL}
    ${QEMU_DISPLAY}
    -usb -device usb-tablet
)
```

QEMU 的启动参数逐个解释。`-m` 指定模拟内存大小，本地开发给 8G 足够。`-serial stdio` 把虚拟机的第一个串口（COM1）映射到宿主终端的 stdin/stdout——这意味着内核里往串口输出的内容（`kprintf`、`serial_printf`）会直接出现在终端里，是调试的核心通道。`-no-reboot` 让 QEMU 在遇到 triple fault 时直接退出而不是重启——否则内核一旦 triple fault 就会无限重启，你永远看不到错误现场。`-debugcon file:debug.log` 把 Bochs 调试端口（I/O port 0xE9）的输出重定向到 `debug.log` 文件，内核可以通过 `outb(0xE9, ch)` 写调试字符，QEMU 自动记录下来。

`-usb -device usb-tablet` 添加 USB 支持和一个 USB tablet 设备——这不是 tag 000 需要的，而是后续 GUI 模式下鼠标输入所需要的。USB tablet 设备提供绝对坐标的鼠标位置，比 PS/2 鼠标的相对位移更适合窗口化的 QEMU 使用。

### QEMU 配置 — Make target 注册

```cmake
set(MBR_BIN    "${CMAKE_BINARY_DIR}/boot/mbr.bin")
set(STAGE2_BIN "${CMAKE_BINARY_DIR}/boot/stage2.bin")
set(MINI_BIN   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel.bin")
set(BIG_KERNEL_BIN "${CMAKE_BINARY_DIR}/kernel/big/big_kernel")

add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
        ${BIG_KERNEL_BIN}
    DEPENDS mbr stage2 mini_kernel big_kernel
    COMMENT "Building disk image: ${CINUX_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(image ALL
    DEPENDS ${CINUX_IMAGE_PATH}
)
```

这段是 `cmake/qemu.cmake` 的核心——它定义了从编译产物到磁盘镜像的依赖链。`add_custom_command` 声明了一个命令：调用 `build_image.sh` 脚本，传入所有编译产物的路径，输出磁盘镜像。`DEPENDS` 列出了依赖的目标——如果这些目标中的任何一个还没构建，CMake 会先构建它们。`add_custom_target(image ALL)` 创建了一个名为 `image` 的顶层 target，加上 `ALL` 让它在默认构建（`make`）时就会被执行。

注意参数的顺序：MBR -> Stage2 -> Mini kernel -> 输出镜像路径 -> Big kernel。Big kernel 是可选的（最后一个参数可以为空），因为 tag 000 只有 MBR，没有 stage2 和内核——但脚本的设计已经考虑了后续 tag 的扩展。

```cmake
add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
        -device ahci,id=ahci
        -drive file=${AHCI_TEST_IMAGE},format=raw,if=none,id=ahci-disk
        -device ide-hd,drive=ahci-disk,bus=ahci.0
        -drive file=${EXT2_IMAGE},format=raw,if=none,id=ext2-disk
        -device ide-hd,drive=ext2-disk,bus=ahci.1
    DEPENDS image ${AHCI_TEST_IMAGE} ${EXT2_IMAGE}
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)
```

`make run` 的定义。除了主磁盘镜像，还挂载了 AHCI 测试磁盘和 ext2 文件系统磁盘——这些在 tag 000 里不会被使用（MBR 存根不做磁盘 I/O），但在后续 tag 的驱动测试中是必需的。AHCI（Advanced Host Controller Interface）是 SATA 控制器的标准接口，ext2 是 Linux 的经典文件系统格式。提前挂载这些磁盘不会影响 MBR 的执行，因为 BIOS 只会从第一个磁盘启动。

### 镜像构建脚本 — 完整流程

`scripts/build_image.sh` 的核心逻辑在 tag 000 里很简单：创建空镜像 -> 写入 MBR -> 验证签名。但脚本的设计已经考虑了后续的扩展——它接收多个参数来写入不同阶段的编译产物。

脚本开头使用 `set -e`（任何命令失败立即退出），然后 `source` 日志模块。路径配置通过命令行参数传入，都有合理的默认值。输入验证部分逐个检查 MBR、Stage2 和 mini kernel 的二进制文件是否存在，不存在就打印错误信息并退出。

磁盘布局常量定义了各组件在镜像中的位置：Stage2 从 LBA 1 开始（紧跟 MBR），最多占 15 个扇区（7.5KB）；mini kernel 从 LBA 16 开始，最大 416KB（受实模式内存布局限制）。这些常量和 MBR 汇编代码里的常量必须完全一致——如果改了一边忘了改另一边，Stage2 就会被写到错误的扇区，MBR 读到的是垃圾数据。

组件大小验证确保 Stage2 和 mini kernel 不超过分配的空间。超过限制会打印详细的错误信息（包括实际大小和最大允许大小），帮助开发者快速定位问题。

实际的写入操作使用 `dd` 命令：`dd if=mbr.bin of=image.img bs=512 count=1 conv=notrunc status=none`。`conv=notrunc` 参数的含义前面已经反复强调——没有它的话 `dd` 会截断输出文件。`status=none` 抑制 dd 的进度输出，让构建日志更干净。

签名验证是最后一步：从镜像偏移 510 读 2 字节，转成十六进制，和 `55aa` 比较。这个验证能捕获两类常见错误：MBR 文件大小不对（不是 512 字节），或者签名位置写错了。

## 设计决策

### 决策：为什么 QEMU 而不是 Bochs / VirtualBox

**问题**: OS 开发可以用多种模拟器，为什么选 QEMU？

**本项目的做法**: 使用 QEMU 作为主要模拟器，配合 `-serial stdio` 和 `-debugcon` 做调试输出。

**备选方案**: Bochs（内置调试器，但慢），VirtualBox（不适合开发调试），真实硬件（最准但调试困难）。

**为什么不选备选方案**: Bochs 的调试器确实很强大（可以单步执行 BIOS 代码、查看 CPU 状态），但它的模拟速度比 QEMU 慢很多——用 KVM 加速的 QEMU 接近原生速度，而 Bochs 是纯软件模拟。VirtualBox 和 VMware 不适合 OS 开发，因为它们对非标准启动行为的支持不如 QEMU 好，而且缺少方便的串口重定向和 GDB 调试功能。QEMU + GDB 的组合在功能上接近 Bochs 调试器，速度上远超 Bochs，是 OS 开发社区的主流选择。

### 决策：磁盘镜像用 raw 格式

**问题**: QEMU 支持多种磁盘镜像格式（raw, qcow2, vmdk），选哪种？

**本项目的做法**: 使用 raw 格式——磁盘镜像就是一个字节级的磁盘拷贝。

**备选方案**: 使用 qcow2（QEMU 的原生格式，支持快照、压缩、稀疏文件）。

**为什么不选备选方案**: Raw 格式最简单——你可以用 `dd` 直接操作它，用 `xxd` 直接查看它，用十六进制编辑器直接修改它。对于教学项目来说，"能看到磁盘上的每一个字节"是巨大的优势。qcow2 虽然功能更丰富，但它的内部结构是一个额外的抽象层，出了问题更难排查。

### 决策：构建脚本用 Shell 而不是 CMake 原生

**问题**: `build_image.sh` 的逻辑可以用 `add_custom_command` 里的 COMMAND 直接写，为什么要单独一个脚本？

**本项目的做法**: 单独维护 `scripts/build_image.sh`，CMake 只负责调用它。

**备选方案**: 把所有 dd 命令直接写在 `cmake/qemu.cmake` 的 `add_custom_command` 里。

**为什么不选备选方案**: Shell 脚本可以做复杂的流程控制（条件判断、循环、错误处理），CMake 的 `add_custom_command` 只支持简单的命令列表。随着项目发展，镜像构建会越来越复杂（验证大小、打印组件摘要、处理可选的大内核），用 Shell 写比用 CMake 写可读性好得多，调试也方便（直接 `bash -x scripts/build_image.sh`）。

## 扩展方向

- 在 QEMU 启动参数里加 `-d int` 记录所有中断事件到日志，调试 triple fault 时非常有用
- 用 `qemu-img info cinux.img` 查看镜像信息，理解 raw 格式的元数据
- 尝试 `-monitor stdio` 把 QEMU monitor 映射到终端，运行时可以检查内存、寄存器状态
- 给 `build_image.sh` 加一个 `--verbose` 模式，打印每一步的 dd 命令和文件大小
- 在脚本里加一个 `--dry-run` 模式，只打印将要执行的命令而不实际执行，
  方便在不破坏现有镜像的情况下验证参数是否正确
- 尝试把 `build_image.sh` 的逻辑拆分为独立的函数
  （`create_blank_image`, `write_mbr`, `write_stage2`, `verify_signature`），
  提高可读性和可测试性

## Tag 000 总体回顾

到这里，tag 000 的所有代码文件都读完了。让我们回顾一下整个 tag 做了什么，
以及每个文件在整个系统中的角色。

tag 000 的目标是"从零到第一个能验证的点"。
我们搭建了三层基础设施：

第一层是工具链和构建系统。
`cmake/toolchain-x86_64.cmake` 定义了 freestanding 编译的完整参数，
`CMakeLists.txt` 编排了项目的目录结构和编译选项，
`cmake/qemu.cmake` 注册了运行和调试的 make target。
这三层把"怎么编译"、"编译什么"、"怎么运行"彻底分离，
后续加新的源文件只需要在对应目录的 CMakeLists.txt 里加一行。

第二层是最小的可验证输出。
`boot/mbr.S` 虽然只有几十行有效代码，
但它验证了从汇编到 QEMU 的完整流水线。
如果这个流水线在任何一步断了
（汇编器 flag 错误、链接脚本地址不对、
objcopy 参数错误、dd 写入位置不对、
QEMU 启动参数错误），
我们都无法在屏幕上看到那个蓝色的 "C"。
"Hello World" 在 OS 开发里的价值远超普通程序。

第三层是持续验证的基础。
`test/framework/test_framework.h` 提供了 `TEST` 和 `ASSERT_*` 宏，
`test/unit/test_smoke.cpp` 验证框架本身工作正常。
这个框架会伴随整个项目的生命周期，
后续每加一个新功能（GDT、IDT、内存管理、文件系统），
都会有对应的 Host 端单元测试来保证逻辑正确性。

从代码量上看，tag 000 是整个项目里最小的——
但它建立的所有基础设施（构建系统、测试框架、开发调试工作流）
会在后续 39 个 tag 里一直使用。
这就是为什么花一整个 tag 来"搭环境"是值得的。

## 参考资料

- QEMU 官方文档 — [System emulation](https://www.qemu.org/docs/master/system/): QEMU 启动参数和设备配置
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 格式和磁盘布局
- OSDev Wiki — [Disk Access using the BIOS](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)): LBA 和 CHS 寻址
- dd(1) 手册页 — `man dd`: `conv=notrunc`、`bs`、`seek`、`skip` 等参数详解
