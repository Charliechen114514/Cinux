---
title: 026-fs-ramdisk-3 · Ramdisk 文件系统
---

# 026-fs-ramdisk-3: 构建集成与测试体系

## 概览

本文是 tag 026 
代码通读的第三篇，聚焦在构建系统（`scripts/embed_binary.sh`、`kernel/CMakeList
s.txt` 变更、`kernel/linker.ld` 
变更）和测试体系（`kernel/test/test_ramdisk.cpp` 和 
`test/unit/test_ramdisk.cpp`）。前两篇我们看了 ustar 格式定义和 Ramdisk 
驱动实现，现在来看这些代码是如何被编译、链接进内核的，以及如何被测试验证的。

关键设计决策一览：使用 shell 脚本封装 objcopy 
的两步转换解决符号命名问题；host 
端测试镜像实现内核纯逻辑函数，在内存中手工构造 ustar 归档进行全面测试。

## 架构图

```
构建管道:
  initrd_contents/* → tar → initrd.tar → embed_binary.sh → initrd.o → 链接器 
→ big_kernel
                     ↑                                              ↑
               系统工具                                      linker.ld 
.initrd 段

测试体系:
  test/unit/test_ramdisk.cpp (host)     kernel/test/test_ramdisk.cpp 
(in-kernel)
    ├─ 镜像 octal_to_uint                  ├─ 真实 octal_to_uint
    ├─ 镜像 is_valid_ustar                  ├─ UstarHeader 大小测试
    ├─ 镜像 data_blocks                     ├─ 真实 Ramdisk::mount()
    ├─ build_ustar_header 辅助               └─ 真实 initrd 归档
    └─ 手工构造归档 → mount_archive
```

## 代码精讲

### embed_binary.sh 脚本

```bash
#!/bin/bash
set -euo pipefail

INPUT="$1"
OUTPUT="$2"
SECTION="$3"
SYM_PREFIX="$4"

OBJCOPY="${OBJCOPY:-objcopy}"
```

脚本接收四个参数：输入文件路径、输出 ELF 
目标文件路径、目标段名、符号前缀。`set -euo pipefail` 
是防御性编程的标配：`-e` 任何命令失败立即退出，`-u` 使用未定义变量时报错，`-o 
pipefail` 管道中任何命令失败都会导致整个管道失败。`OBJCOPY` 
环境变量支持交叉编译工具链（比如 `x86_64-elf-objcopy`）。

第一步转换：

```bash
"${OBJCOPY}" \
    -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data="${SECTION}",CONTENTS,ALLOC,LOAD,READONLY,DATA \
    "${INPUT}" "${OUTPUT}"
```

`-I binary` 告诉 objcopy 把输入当作原始二进制数据。`-O elf64-x86-64` 
指定输出格式为 x86-64 ELF。`-B i386:x86-64` 
设置二进制架构。`--rename-section` 把默认的 `.data` 段改名为我们指定的 
`.initrd`，同时设置段属性：CONTENTS（有实际内容）、ALLOC（占用内存）、LOAD（需
要加载）、READONLY（只读）、DATA（数据段）。

第二步符号重命名：

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

先用 `nm` 
从第一步生成的目标文件中提取自动生成的符号名（那些又长又不稳定的路径派生名称）
，然后用 objcopy 的 `--redefine-sym` 把它们改成稳定的名字。比如自动生成的 
`_binary__home_charlie_cinux_kernel_data_initrd_tar_start` 被重命名为 
`_binary_initrd_start`。

这个两步走的设计是必要的，因为 objcopy 
不提供直接指定输出符号名的选项——它只能从输入路径推导。

### CMake 构建集成

```cmake
set(INITRD_TAR ${CMAKE_CURRENT_SOURCE_DIR}/data/initrd.tar)
set(INITRD_OBJ  ${CMAKE_BINARY_DIR}/kernel/initrd.o)
set(EMBED_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/embed_binary.sh)

add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND bash ${EMBED_SCRIPT}
        ${INITRD_TAR} ${INITRD_OBJ} .initrd _binary_initrd
    DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}
    COMMENT "Converting initrd.tar -> initrd.o (embedded ramdisk)"
)

add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})
```

`add_custom_command` 定义了一个构建规则：当 `${INITRD_OBJ}` 不存在或者 
`${INITRD_TAR}` / `${EMBED_SCRIPT}` 被修改时，执行转换命令。`COMMENT` 
提供了构建日志中的可读描述。

`add_custom_target` 创建了一个虚拟目标 
`initrd_obj`，它本身不产生文件，只是声明对 `${INITRD_OBJ}` 
的依赖。这种间接层是 CMake 的惯用模式——`add_custom_command` 的 OUTPUT 
只有在被其他目标依赖时才会执行，`add_custom_target` 就是那个"其他人"。

链接到内核可执行文件：

```cmake
add_dependencies(big_kernel user_binary_obj initrd_obj)
target_sources(big_kernel PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)
```

`add_dependencies` 确保 initrd_obj 在 big_kernel 之前构建。`target_sources` 
把 initrd.o 加入链接列表。test 可执行文件也做了同样的处理。

### 链接脚本变更

```
.initrd : AT(ADDR(.initrd) - KERNEL_VMA) ALIGN(4096) {
    *(.initrd)
}
```

新段放在 `.init_array` 和 `.bss` 之间。`ALIGN(4096)` 是 4KB 
页对齐。`AT(ADDR(.initrd) - KERNEL_VMA)` 设置加载地址（LMA）——因为 Cinux 是 
higher-half 内核，虚拟地址和物理地址之间有 `KERNEL_VMA` 的偏移量。`AT()` 
指令告诉链接器"虽然这个段的运行时地址是 VMA，但把它放在 LMA 
对应的文件偏移处"。这样 bootloader 加载内核时，initrd 
数据会被加载到正确的物理地址。

通配符 `*(.initrd)` 匹配所有输入目标文件中名为 `.initrd` 的段。由于只有 
initrd.o 包含这个段名（由 embed_binary.sh 的 `--rename-section` 
设置），所以恰好只会把 initrd 数据放进来。

### 内核集成测试

`kernel/test/test_ramdisk.cpp` 是在 QEMU 
中运行的内核模式集成测试。它验证三件事：UstarHeader 大小等于 
512、octal_to_uint 正确解析各种八进制字符串、真实的 initrd 
归档能成功挂载并找到 3 个文件。

测试使用 Cinux 已有的 `TEST_ASSERT_*` 
宏。值得注意的是最后一个测试：`test_ramdisk_mount_finds_files` 调用真实的 
`Ramdisk::mount()`，验证返回值等于 3（因为 initrd.tar 包含 
hello.txt、readme.txt、etc/passwd 三个文件）。这个测试的前提是 initrd 
确实被链接进了 test 内核——CMake 中 big_kernel_test 也依赖 initrd_obj 
保证了这一点。

### Host 端单元测试

`test/unit/test_ramdisk.cpp` 是 867 行的全面测试，运行在主机上而非 QEMU 
中。它的核心设计挑战是：内核代码依赖 kprintf 和链接器符号，这些在 host 
环境中不可用。解决方案是在测试文件的 `ramdisk_test` 
命名空间中镜像实现所有纯逻辑函数。

测试按前缀分为约 12 个概念组，共 73 个用例：

ramdisk_ustar_header (7): UstarHeader 结构体布局（sizeof、各字段偏移量）
ramdisk_ustar_type (7): UstarType 常量值（'0'-'7'）
ramdisk_magic (2): USTAR_MAGIC 和 USTAR_BLOCK_SIZE
ramdisk_octal (~16): octal_to_uint 的各种输入（单数字、多位、null终止、空格终止、边界）
ramdisk_valid_ustar (6): is_valid_ustar（合法、非法、空、部分 magic）
ramdisk_data_blocks (8): data_blocks（零、单块、对齐、非对齐、大文件）
ramdisk_mount (~14): mount_archive 的各种场景（单文件、目录、混合、空归档、错误 magic、非对齐大小）
ramdisk_entry (1): RamdiskEntry 结构体字段
ramdisk_name_max (1): RAMDISK_NAME_MAX 常量
ramdisk_*_data_driven (2): 批量已知值测试

`build_ustar_header` 辅助函数可以在调用者提供的缓冲区中构造有效的 ustar 
头部，支持指定文件名、大小和类型。`mount_archive` 函数模拟 Ramdisk::mount 
的遍历逻辑，返回 MountResult 结构体（包含文件计数、目录计数和是否遇到坏 
magic）。

最后的数据驱动测试用表驱动方式批量验证多个已知值，减少重复代码同时保证覆盖率。

## 设计决策

### 决策：host 测试镜像实现而非 mock

**问题**: 如何在 host 端测试内核的纯逻辑函数？**本项目的做法**: 
在测试文件中镜像拷贝纯逻辑实现（octal_to_uint、is_valid_ustar、data_blocks、mo
unt_archive）。**备选方案**: 使用 mock 框架替换 kprintf 
和链接器符号。**为什么不选备选方案**: Cinux 的测试框架不支持 
mock。镜像实现虽然意味着测试不直接测内核代码，但这些函数逻辑简单，镜像实现和原
实现差异很小，测试的主要价值在于验证算法正确性而非代码集成。

## 扩展方向

- 添加 CI 集成：确保 host 测试和内核测试都在 CI 中运行。
- 添加 initrd 内容的自动化生成：CMake 自定义命令根据配置文件动态生成 
initrd.tar。
- 添加 ramdisk 压力测试：构造大型归档（数千条目、大文件）验证性能和正确性。
- 添加嵌入式资源的通用化：将 embed_binary.sh 泛化为可复用的 CMake 
模块，支持嵌入任意二进制资源。

## 参考资料

- OSDev Wiki: [Initrd](https://wiki.osdev.org/Initrd) — 嵌入方法对比
- GNU Binutils: [objcopy 
manual](https://sourceware.org/binutils/docs/binutils/objcopy.html) — `-I 
binary` 选项和符号重定义
- CMake: 
[add_custom_command](https://cmake.org/cmake/help/latest/command/add_custom_co
mmand.html) — 自定义构建规则
- GNU LD: [Linker 
Scripts](https://sourceware.org/binutils/docs/ld/Scripts.html) — AT() 
指令和段属性
