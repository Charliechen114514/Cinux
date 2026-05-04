# 把文件塞进内核——二进制嵌入的艺术

> 标签：objcopy, ELF, 链接器脚本, CMake, 二进制嵌入, initrd
> 前置：[026-fs-ramdisk-1](026-fs-ramdisk-1.md)

## 前言

上一篇我们把 ustar 格式的定义写好了——512 字节的头部结构体、类型常量、magic 
字符串、八进制转换函数。但光有"图纸"没有用，我们还需要实际的 initrd 
数据。问题是：内核是一个 ELF 格式的可执行文件，initrd 是一个 tar 
格式的归档文件——两者完全不是一个世界的东西。怎么把 tar 文件"嵌"进内核里？

说实在的，这在嵌入式和 OS 开发领域是一个经典问题，而且有一个经典答案：GNU 
objcopy 的 `-I binary` 模式。这个功能可以把任意二进制文件包装成 ELF 
目标文件，然后链接进可执行程序。听起来简单，但中间有一个大坑——符号命名问题。这
一篇我们就来详细拆解整个嵌入管道：从 tar 
文件到链接器符号，每一步发生了什么，为什么要这样做。

## 环境说明

工具链：GNU Binutils (objcopy, nm, ld)，CMake 3.20+，bash。内核链接脚本使用 
GNU LD 语法。内核是 higher-half 设计，运行在虚拟地址 `0xFFFFFFFF80000000` 
以上，所以链接脚本中需要处理 VMA/LMA 的映射。

## 第一步——准备 initrd 内容

在动手搞嵌入之前，我们先准备要嵌入的内容。在 `kernel/data/initrd_contents/` 
目录下放三个简单的测试文件：

- `hello.txt`：包含一行 "Hello from Cinux!"
- `readme.txt`：包含一行 "This is a readme file."
- `etc/passwd`：包含一行 "root:x:0:0"

然后用系统 tar 命令打包：

```bash
cd kernel/data
tar cf initrd.tar -C initrd_contents .
```

注意 `-C initrd_contents` 配合 `.` 确保归档内的路径是相对的——`hello.txt` 
而不是 
`/home/charlie/cinux/kernel/data/initrd_contents/hello.txt`。这个细节很重要，
因为 ustar 头部的 name 字段只有 100 字节，绝对路径很容易超限。

可以用 `tar -tvf initrd.tar` 验证归档内容。你应该看到类似这样的输出：

```
drwxr-xr-x  0/0               0 2026-05-03 00:00 ./
-rw-r--r--  0/0              20 2026-05-03 00:00 hello.txt
-rw-r--r--  0/0              24 2026-05-03 00:00 readme.txt
drwxr-xr-x  0/0               0 2026-05-03 00:00 etc/
-rw-r--r--  0/0              13 2026-05-03 00:00 etc/passwd
```

## 第二步——objcopy 的二进制转换魔法

现在来看整个管道中最关键的一步：把 `initrd.tar` 变成 `initrd.o`。GNU objcopy 
有一个不太为人知但极其实用的功能。当你用 `-I binary` 
告诉它"输入是一个原始二进制文件"时，它会做三件事：把文件内容放到输出 ELF 
文件的 `.data` 
段中；自动生成三个全局符号——`_binary_<路径>_start`、`_binary_<路径>_end`、`_bi
nary_<路径>_size`；设置好段属性使数据可以被加载和访问。

这些自动生成的符号正是我们在内核代码中访问 initrd 数据的桥梁——`_start` 
指向数据起始，`_end` 指向数据末尾，两者相减就是数据大小。

但问题马上就来了：符号名中的 `<路径>` 
部分是从输入文件的**绝对路径**推导出来的，把所有非字母数字字符替换成下划线。如
果你在 `/home/charlie/cinux/kernel/data/initrd.tar` 运行 
objcopy，生成的符号会变成 
`_binary__home_charlie_cinux_kernel_data_initrd_tar_start`——又长又丑，而且不同
开发者的路径不同，CI 环境的路径也不同。这意味着内核代码中 `extern` 
声明的符号名和实际生成的符号名对不上，直接链接失败。

## 第三步——符号重命名的两步走

解决方案是写一个 shell 脚本 `scripts/embed_binary.sh`，分两步走：

第一步用 objcopy 的 `-I binary` 做初始转换，同时用 `--rename-section` 
把默认的 `.data` 段改名为 `.initrd`。段名很重要——我们在链接脚本中会用 
`*(.initrd)` 来匹配这个段，如果还叫 `.data` 就会和内核的其他数据段混在一起。

```bash
"${OBJCOPY}" \
    -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data="${SECTION}",CONTENTS,ALLOC,LOAD,READONLY,DATA \
    "${INPUT}" "${OUTPUT}"
```

段属性设为 `READONLY` 是因为 initrd 
数据在运行时只读——不应该被修改。`ALLOC,LOAD` 
确保数据被加载到内存中（不会因为"未使用"被优化掉）。

第二步提取自动生成的符号名，然后用 objcopy 的 `--redefine-sym` 重命名：

```bash
SYM_START=$(nm "${OUTPUT}" | grep '_start$' | awk '{print $3}')
SYM_END=$(nm "${OUTPUT}" | grep '_end$' | awk '{print $3}')
SYM_SIZE=$(nm "${OUTPUT}" | grep '_size$' | awk '{print $3}')

"${OBJCOPY}" \
    --redefine-sym "${SYM_START}=${SYM_PREFIX}_start" \
    --redefine-sym "${SYM_END}=${SYM_PREFIX}_end" \
    --redefine-sym "${SYM_SIZE}=${SYM_PREFIX}_size" \
    "${OUTPUT}"
```

`nm` 命令列出目标文件中的符号，`grep` 找到以 `_start`/`_end`/`_size` 
结尾的，`awk` 提取第三列（符号名）。然后逐个重命名为稳定的 
`_binary_initrd_start`、`_binary_initrd_end`、`_binary_initrd_size`。这样无论
源码树在哪个目录下，生成的符号名都是一样的。

## 第四步——CMake 构建规则

有了转换脚本，接下来把它集成进 CMake。Cinux 使用 `add_custom_command` 
定义转换规则：

```cmake
set(INITRD_TAR ${CMAKE_CURRENT_SOURCE_DIR}/data/initrd.tar)
set(INITRD_OBJ  ${CMAKE_BINARY_DIR}/kernel/initrd.o)

add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND bash ${EMBED_SCRIPT}
        ${INITRD_TAR} ${INITRD_OBJ} .initrd _binary_initrd
    DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}
    COMMENT "Converting initrd.tar -> initrd.o (embedded ramdisk)"
)

add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})
```

`DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}` 确保如果修改了 initrd 
内容或嵌入脚本，转换命令会重新执行。`add_custom_target` 
创建了一个虚拟目标作为依赖桥梁——只有被其他目标显式依赖时，custom_command 
才会真正执行。

然后在 big_kernel 和 big_kernel_test 的链接列表中加入 initrd.o：

```cmake
add_dependencies(big_kernel user_binary_obj initrd_obj)
target_sources(big_kernel PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)
```

## 第五步——链接脚本中的 .initrd 段

最后一步是在链接脚本中给 initrd 数据安排一个位置。Cinux 在 `kernel/linker.ld` 
中新增了 `.initrd` 段：

```
.initrd : AT(ADDR(.initrd) - KERNEL_VMA) ALIGN(4096) {
    *(.initrd)
}
```

这个段放在 `.init_array` 和 `.bss` 之间，4KB 页对齐。`AT(ADDR(.initrd) - 
KERNEL_VMA)` 是关键——因为 Cinux 是 higher-half 内核，虚拟地址比物理地址高 
`KERNEL_VMA`（`0xFFFFFFFF80000000`）。`AT()` 指令设置 
LMA（加载内存地址），告诉链接器"虽然这个段的运行地址是 VMA，但把它放在 LMA 
对应的文件偏移处"。如果不设 AT()，bootloader 会在错误的物理地址加载 initrd 
数据。

`*(.initrd)` 通配符匹配所有输入目标文件中名为 `.initrd` 
的段——在我们的构建中，只有 `initrd.o` 包含这个段名（由 embed_binary.sh 的 
`--rename-section` 设置）。

## 其他 OS 怎么嵌入 initrd？

**Linux** 使用两种方式嵌入 initramfs。一种是编译时嵌入：通过 
`CONFIG_INITRAMFS_SOURCE` Kconfig 选项，内核构建系统自动把指定的 cpio 
归档压缩后嵌入内核镜像——底层机制和 Cinux 
类似，也是通过链接器把数据编译进内核。另一种是运行时加载：bootloader 通过 
initrd 协议把 initrd 加载到物理内存中的指定位置，内核启动后通过 boot protocol 
获取地址。Linux 两种方式都支持，灵活性更高。

**xv6** 的做法更直接：QEMU 的 `-rdinit fs.img` 
选项直接把文件系统镜像加载到内存的固定地址（在 xv6 的 `memlayout.h` 中定义为 
`RAMDISK`），内核通过硬编码地址访问。这比 Cinux 的 objcopy 
方案更简单，但灵活性差——改地址要重新编译内核。

Cinux 选择 objcopy 嵌入方案的原因是：我们的自写 bootloader 不像 GRUB 
那样支持多模块加载，也不像 QEMU 那样能直接注入内存。把 initrd 
编译进内核是最简单、最可靠的方案——只要内核能跑起来，initrd 就一定在。

## 收尾

到这里，initrd 数据已经成功嵌入内核镜像了。构建后用 `nm` 检查内核 ELF 
文件，你会看到 `_binary_initrd_start` 和 `_binary_initrd_end` 
两个符号，它们的差值就是 initrd.tar 的大小（10240 字节）。下一篇我们终于要写 
Ramdisk 驱动了——通过这些符号找到 initrd 数据，解析 ustar 
格式，在串口打印出所有文件的名字和大小。

## 参考资料

- GNU Binutils: [objcopy 
manual](https://sourceware.org/binutils/docs/binutils/objcopy.html) — `-I 
binary` 和 `--redefine-sym`
- GNU LD: [Linker 
Scripts](https://sourceware.org/binutils/docs/ld/Scripts.html) — AT() LMA 指令
- OSDev Wiki: [Initrd](https://wiki.osdev.org/Initrd) — 嵌入方法对比
- Linux kernel: 
[initramfs](https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html) 
— Linux 的 initramfs 嵌入机制
