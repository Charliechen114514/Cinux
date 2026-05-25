---
title: 000-env-toolchain-1 · 环境搭建
---

# 000-1 · 构建系统与工具链

## 概览

本文是 tag `000_env_toolchain` 的第一篇通读，聚焦整个构建基础设施——从顶层 CMakeLists.txt 到 toolchain file、QEMU 集成、boot 子目录的编译配置、以及辅助脚本。这些文件在功能上是"胶水代码"：它们本身不实现任何 OS 功能，但没有它们，后面的 bootloader 和内核根本编译不起来。我们会逐个文件走一遍，搞清楚每一行配置的作用和背后的设计考量。注意，本文展示的是 tag 000 时刻的代码快照——随着后续 tag 的推进，部分文件（特别是 `boot/CMakeLists.txt` 和链接脚本）会被扩展。

## 架构图

```
                    Cinux 构建架构

cmake/toolchain-x86_64.cmake     ← 编译 flag 注入
           │
           ▼
    CMakeLists.txt (顶层)         ← 全局配置 + 子目录编排
    ├── add_subdirectory(boot)    ← MBR 编译 → mbr.bin
    ├── add_subdirectory(kernel)  ← 内核编译（后续 tag）
    ├── add_subdirectory(test)    ← Host 端单元测试
    └── include(cmake/qemu.cmake) ← make run/run-debug target

                    编译流水线 (以 MBR 为例)

    mbr.S ──→ mbr.o ──→ mbr.elf ──→ mbr.bin ──→ cinux.img
     (汇编)   (AS)    (ld + linkerscript) (objcopy)  (dd)

                    辅助脚本

    scripts/check_toolchain.sh  → 验证工具安装
    scripts/log/logging.sh      → 带颜色的日志函数
    scripts/build_image.sh      → dd 组装磁盘镜像
```

## 代码精讲

### 顶层 CMakeLists.txt — 项目全局配置

```cmake
cmake_minimum_required(VERSION 4.1)

project(cinux
    VERSION 0.1.0
    LANGUAGES C CXX ASM)
```

`cmake_minimum_required(VERSION 4.1)` 设定了 CMake 最低版本要求——这里用的是笔者开发环境安装的 CMake 4.1。实际最低需要 3.20 以上就能支持现代 CMake 特性（toolchain file、generator expressions 等），读者可以根据自己的环境调整这个版本号。`project()` 声明了项目名称、版本和使用的语言。`LANGUAGES C CXX ASM` 告诉 CMake 我们会同时编译 C、C++ 和汇编文件——CMake 会分别为它们检测编译器。

```cmake
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64.cmake"
        CACHE FILEPATH "Toolchain file")
    message(STATUS "Using default toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()
```

这段代码实现了一个便捷的默认值机制：如果用户没有在命令行通过 `-DCMAKE_TOOLCHAIN_FILE=...` 指定 toolchain file，就自动使用项目里的 `cmake/toolchain-x86_64.cmake`。`CACHE FILEPATH` 把这个值写入 CMake cache，这样后续增量构建时不需要重新检测。不过这里有一个微妙的时序问题——toolchain file 是在 `project()` 命令执行时被读取的，而这段代码在 `project()` 之后，所以它实际上只在第一次 configure 时生效（CMake 会在后续运行时从 cache 里读取）。在实际使用中这不是问题，因为 toolchain file 的路径一旦设定就不会变。

```cmake
add_compile_options(
    -Wall
    -Wextra
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
```

`-Wall -Wextra` 是 GCC 的基础警告选项，对所有目标生效。`CMAKE_EXPORT_COMPILE_COMMANDS ON` 生成 `compile_commands.json`——这个文件是 clangd（VS Code 的 C++ 语言服务器）正常工作的前提，没有它 IDE 里的代码补全和错误提示基本是瞎的。Debug/Release 模式的编译 flag 设置是标准做法：Debug 模式关优化加调试信息（`-g -O0`），Release 模式开优化并定义 `NDEBUG` 宏（`-O2 -DNDEBUG`）。

```cmake
add_subdirectory(boot)
add_subdirectory(kernel)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()

include(cmake/qemu.cmake)
```

最后是子目录引入。`boot` 和 `kernel` 是必须的，`test` 通过 `CINUX_BUILD_TESTS` option 控制是否引入（默认不引入，需要用户手动 `-DCINUX_BUILD_TESTS=ON` 开启），这样在发布构建或者不需要跑测试的时候不会编译测试代码。`include(cmake/qemu.cmake)` 把 QEMU 相关的 make target 注册进来。

### Toolchain File — 裸机编译的核心

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
```

这两行是 toolchain file 最重要的配置。`CMAKE_SYSTEM_NAME Generic` 告诉 CMake"目标平台没有操作系统"——这会触发 CMake 的一系列特殊行为：禁用所有 `find_package` 和 `find_library` 的默认搜索路径（因为没有系统库可以找），不允许尝试运行编译出的目标程序（因为交叉编译产物不能在宿主机上执行），等等。如果你不小心写成了 `Linux` 或者留空，CMake 会按照 Linux 程序的方式来配置构建，到后面各种链接错误会让你怀疑人生。

`CMAKE_SYSTEM_PROCESSOR x86_64` 描述目标处理器架构。这个值主要影响一些 CMake 内部的条件判断和 `try_compile` 行为。

```cmake
set(CMAKE_C_FLAGS_INIT "
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
    -mcmodel=kernel
    -Wall
    -Wextra
")

set(CMAKE_CXX_FLAGS_INIT ${CMAKE_C_FLAGS_INIT} "
    -fno-exceptions
    -fno-rtti
    -std=c++23
")

set(CMAKE_ASM_FLAGS_INIT "-Wa,--divide")

set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib
    -static
    -Wl,--build-id=none
")
```

这里使用 `CMAKE_*_FLAGS_INIT` 而不是 `CMAKE_*_FLAGS`，两者有一个重要区别：`_INIT` 变量只在第一次 configure 时被读取，之后用户可以通过 `-DCMAKE_C_FLAGS=...` 覆盖而不会和 toolchain file 里的 flag 冲突；直接设 `CMAKE_C_FLAGS` 则会阻止用户追加自定义 flag。所以 `_INIT` 是 toolchain file 的推荐写法。

逐个 flag 的含义：`-ffreestanding` 告诉编译器代码运行在无 OS 环境，只提供 freestanding 头文件（`<stdint.h>`, `<stddef.h>` 等）。`-fno-stack-protector` 禁用栈保护（因为栈保护的 canary 初始化依赖 `__stack_chk_fail`，这个函数需要标准库提供）。`-mno-red-zone` 禁用 x86_64 的 red zone——这是 System V ABI 给用户态程序留的 128 字节栈下空间优化，内核代码绝对不能有，因为中断可能在任何时候打断执行，如果用了 red zone 里的空间，中断处理程序的栈帧会覆盖掉还没用完的数据。`-mcmodel=kernel` 告诉编译器代码会跑在地址空间的最顶端（高半核，`0xFFFFFFFF80000000` 附近），影响它生成绝对地址引用的方式。

C++ 特有的 flag：`-fno-exceptions` 禁用异常（异常需要 `.eh_frame` 段和 `__cxa_begin_catch` 等运行时支持，内核里都没有），`-fno-rtti` 禁用 RTTI（`dynamic_cast` 和 `typeid` 需要额外元数据），`-std=c++23` 选择最新的 C++ 标准。

汇编器 flag `-Wa,--divide` 允许汇编代码里直接写除法符号 `/` 而不被 GNU AS 误解析（某些版本的 GNU AS 默认把 `/` 当注释符）。链接器 flag `-nostdlib` 不链接标准启动文件和库，`-static` 纯静态链接不依赖动态链接器。

```cmake
set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

最后三行设置查找策略：`PROGRAM NEVER` 意味着 `find_program` 只在宿主系统搜索（我们需要在宿主上找 CMake、objcopy 这些工具），`LIBRARY ONLY` 和 `INCLUDE ONLY` 意味着 `find_library` 和 `find_path` 只在 `CMAKE_FIND_ROOT_PATH` 下搜索——因为我们设的 `FIND_ROOT_PATH` 是空的，效果就是禁用了所有库和头文件的自动查找，内核项目必须手动指定所有依赖。

### Boot CMakeLists.txt — MBR 编译配置（tag 000 版本）

```cmake
add_executable(mbr
    mbr.S
)

target_compile_options(mbr PRIVATE
    -Wa,--32                    # Assemble as 32-bit (allows 16-bit code)
)

target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)

add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
    VERBATIM
)
```

MBR 的编译分三步：汇编成 ELF 目标文件（`add_executable` 自动调用 AS），链接成 ELF 可执行文件（`target_link_options` 指定链接脚本和 `-nostdlib`），然后 `add_custom_command(POST_BUILD)` 在链接完成后自动执行 `objcopy -O binary` 把 ELF 转成裸二进制。`-Wa,--32` 让汇编器生成 32 位目标文件——这是必要的，因为 16 位代码（`.code16`）在 32 位 ELF 里编码兼容性最好。`-Wl,-m,elf_i386` 指定链接器使用 32 位 i386 ELF 格式，而 `-no-pie` 禁用位置无关可执行文件——MBR 运行在固定地址 `0x7C00`，不需要也不允许 PIC 重定位。为什么要转裸二进制？因为 BIOS 不认识 ELF 格式——它只认磁盘上的原始字节，ELF 文件头、段头表这些元数据对 BIOS 来说是垃圾数据。`objcopy -O binary` 会去掉所有元数据，只保留代码和数据的原始内容。

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf64-x86-64\")
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

链接脚本在 CMake configure 时通过 `file(WRITE)` 生成到 build 目录。`. = 0x7C00` 设置起始虚拟地址——这让链接器知道代码应该从 `0x7C00` 开始布局，这样 `objcopy` 输出的裸二进制就是 BIOS 期望的格式。`OUTPUT_FORMAT("elf64-x86-64")` 指定 64 位 ELF 格式——虽然 MBR 里是 16 位代码（`.code16`），但链接器在 elf64 目标文件中同样能正确处理 16 位指令编码，且与后续 64 位内核的编译工具链保持一致。`.rodata` 被合并到 `.text` 段里，这样 MBR 里嵌入的字符串常量（如启动消息）会和代码放在一起，方便 `objcopy` 一次性提取。`/DISCARD/` 段丢掉编译器自动生成的 `.comment` 和 `.note` 段，它们对裸机执行没有意义，留着只会浪费宝贵的 512 字节空间。

### 辅助脚本 — 日志模块

```bash
export LOG_RED='\033[0;31m'
export LOG_GREEN='\033[0;32m'
export LOG_YELLOW='\033[1;33m'
export LOG_NC='\033[0m'

log_info() {
    echo -e "${LOG_GREEN}[INFO]${LOG_NC} $1"
}

log_error() {
    echo -e "${LOG_RED}[ERROR]${LOG_NC} $1" >&2
}

log_success() {
    echo -e "${LOG_GREEN}[SUCCESS]${LOG_NC} $1"
}
```

日志模块是一个很小的 Shell 工具库，用 ANSI 转义序列给终端输出加上颜色标记。`log_info` 打绿色 `[INFO]`，`log_error` 打红色 `[ERROR]` 并重定向到 stderr，`log_success` 打绿色 `[SUCCESS]`。通过 `source` 命令引入到其他脚本中复用。虽然简单，但统一的日志格式让构建过程的输出更容易扫读。

### 辅助脚本 — 工具链检查

```bash
check_command() {
    local cmd="$1"
    local install_hint="$2"
    if command -v "$cmd" &> /dev/null; then
        log_info "[OK] $cmd found"
        return 0
    else
        log_error "[MISSING] $cmd not found"
        if [[ -n "$install_hint" ]]; then
            log_error "  Install: $install_hint"
        fi
        exit 1
    fi
}
```

`check_command` 是一个通用的工具检测函数：用 `command -v` 判断命令是否存在于 PATH 中，存在就打 `[OK]`，不存在就打 `[MISSING]` 并附带安装提示后退出。`exit 1` 是关键——任何一个工具缺失都会终止脚本，而不是继续往下检查，因为后续工具的检测可能依赖前面的工具（比如 CMake 的版本检测需要 CMake 已经安装）。

主检查逻辑依次调用 `check_command gcc`, `check_command g++`, `check_command as`, `check_command ld`, `check_command objcopy`, `check_command qemu-system-x86_64`，最后单独检查 CMake 版本——用 `cmake --version | head -n1` 提取版本号，再和最低版本 `4.1` 做数值比较。版本检查单独处理是因为 `command -v cmake` 只能告诉你 CMake 装没装，不能告诉你版本够不够。

### 辅助脚本 — 镜像构建（tag 000 版本核心逻辑）

tag 000 的 `build_image.sh` 核心逻辑只有几步：用 `dd if=/dev/zero` 创建一个全零的空白镜像文件（1MB 足够），然后 `dd if=mbr.bin of=image.img bs=512 count=1 conv=notrunc` 把 MBR 裸二进制写入第 0 扇区。`conv=notrunc` 这个参数前面踩坑预警里提过了——没有它的话 dd 会把输出文件截断到和输入一样长，后续写入 stage2 的时候就全毁了。

验证步骤从镜像偏移 510 处读 2 字节，转成十六进制和 `55aa` 比较。这个验证虽然简单，但能捕获两类常见错误：MBR 文件大小不对（不是 512 字节），或者签名位置写错了。

## 设计决策

### 决策：系统 GCC vs 交叉编译器

**问题**: OS 内核开发通常推荐构建专用的交叉编译器（`x86_64-elf-gcc`），但构建过程耗时且容易出错。Cinux 选择直接用系统 GCC。

**本项目的做法**: 使用系统 GCC + CMake toolchain file 注入 freestanding flag。因为宿主和目标都是 x86_64，指令集完全兼容。

**备选方案**: 用 crosstool-NG 或手动构建 `x86_64-elf-gcc` 交叉编译器。

**为什么不选备选方案**: 交叉编译器构建需要 30 分钟以上，而且可能因为依赖缺失而失败。对于 x86_64 → x86_64 这种"本地交叉编译"场景，系统 GCC 加正确 flag 已经足够。真正的风险在于 GCC 可能隐式假设某些宿主环境特性（比如默认的 `--dynamic-linker`），但这些都被 `-nostdlib -static` 显式关闭了。

**如果要改进**: 可以在 CI 里加一个检查，确保链接后的 ELF 不依赖任何动态库（`readelf -d kernel.elf | grep NEEDED` 应该为空），作为系统 GCC 方案的额外安全网。

### 决策：CMake 而不是 Makefile

**问题**: 大多数 OS 教程用裸 Makefile（包括 xv6、ToaruOS），CMake 带来了额外复杂度。

**本项目的做法**: 使用 CMake 作为构建系统，配合 toolchain file 和自定义 target。

**备选方案**: 用一个简单的 Makefile，直接硬编码编译器和 flag。

**为什么不选备选方案**: Makefile 的可扩展性很差——当项目从 2 个源文件增长到 50 个时，Makefile 会变成一坨难以维护的规则。CMake 的 `add_executable`/`add_library` + 自动依赖追踪能自动处理头文件依赖关系。而且 CMake 的 toolchain file 机制是处理交叉编译的标准方案，比 Makefile 里手写 `CC=` 和 `CFLAGS=` 更干净。至于"额外的复杂度"——CMake 的学习曲线确实比 Makefile 陡，但对于一个会持续开发 40 个 tag 的项目来说，这笔投资是值得的。

**如果要改进**: 可以引入 CMake Presets（`CMakePresets.json`），把常用的 configure 配置（Debug/Release、是否开测试、QEMU 参数等）封装成预设，用户只需要 `cmake --preset debug` 一条命令。

### 决策：链接脚本用 file(WRITE) 而不是独立文件

**问题**: MBR 的链接脚本 `mbr.ld` 只有几行，是单独维护一个 `.ld` 文件还是内联在 CMakeLists.txt 里？

**本项目的做法**: 用 `file(WRITE)` 在 configure 时生成到 build 目录。

**备选方案**: 创建独立的 `boot/mbr.ld` 文件，在 CMake 里用 `-T ${CMAKE_CURRENT_SOURCE_DIR}/mbr.ld` 引用。

**为什么不选备选方案**: tag 000 的链接脚本非常短（10 行左右），而且内容完全由 CMake 变量决定（比如输出格式），单独维护一个文件反而是过度设计。后续 tag 的 stage2 和 kernel 链接脚本会变复杂（涉及 memory region、多段布局、对齐约束），那时候再用独立文件。

## 扩展方向

- ⭐ 在 toolchain file 里加 `-Werror` 把所有警告变成错误，强制保持代码质量
- ⭐ 把 `CMAKE_SYSTEM_NAME` 改成 `Linux` 然后 `make`，观察链接错误的变化，理解为什么必须是 `Generic`
- ⭐⭐ 写一个 CMake Presets 文件封装 Debug/Release 配置，练习现代 CMake 的工作流
- ⭐⭐ 给 `build_image.sh` 加 stage2 写入逻辑（第二个 `dd` 命令），为 tag 001 做准备
- ⭐⭐⭐ 尝试把构建系统迁移到用 `add_custom_command` 生成依赖图，让 CMake 自动跟踪 `mbr.bin` → `cinux.img` 的依赖关系，修改源码后只需要 `make` 就能自动重跑 objcopy + dd

## 参考资料

- CMake 官方文档 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html): toolchain file 的所有变量和用法
- OSDev Wiki — [Bare Bones](https://wiki.osdev.org/Bare_Bones): freestanding 编译 flag 和交叉编译基础
- OSDev Wiki — [MBR (x86)](https://wiki.osdev.org/MBR_(x86)): MBR 结构和 BIOS 引导行为
- Intel SDM — [Volume 1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Basic Architecture，`-mcmodel=kernel` 和 `-mno-red-zone` 的底层原因
- SerenityOS — [CMake 构建系统](https://github.com/SerenityOS/serenity): 生产级 OS 的 CMake 架构参考（比 Cinux 复杂得多）
