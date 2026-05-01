# 009-3 Read-through: ELF 加载器 Bug 修复 + 集成测试 + 压力测试

## 概览

本文是 tag 009 的第三篇 read-through，聚焦于 mini kernel 端的改动——ELF 加载器的一个关键 bug 修复、集成测试框架、CRC32 校验、以及 1GB 压力测试。这个 bug 是整个 tag 中最隐蔽的问题：当大内核的 PT_LOAD 段的 `p_paddr` 与 staging buffer 地址重叠时，`memcpy` 会自毁式地覆盖 ELF 头和 Program Header，导致后续迭代读取垃圾数据而 triple fault。

## 架构图

```
集成测试数据流:

磁盘镜像:
  MBR | Stage2 | mini_kernel_test.bin | big_kernel.elf (带 CRC32)
                ↓ QEMU 加载
          mini kernel test 启动
                ↓ ATA 读扇区
          staging buffer @ 0x1000000
          (big_kernel.elf 的完整内容)
                ↓ load_elf()
          保存 ELF 头 → 遍历 saved_phdrs[]
          memmove 段数据到 p_paddr
          (可能覆盖 staging buffer 自身!)
                ↓ 验证
          入口点 == 0x1000000?
          第一条指令 == 0xFA (cli)?
                ↓ QEMU isa-debug-exit
          exit(1) = PASS

压力测试数据流:

generate_large_elf.py → stress_kernel.elf (1GB)
                ↓ append_crc32.py
          stress_kernel_with_crc.elf
                ↓ build_image.sh
          cinux_stress_test.img
                ↓ QEMU
          mini kernel 加载 1GB ELF
          (~2000 扇区 ATA 读取)
                ↓ isa-debug-exit
          exit(1) = PASS
```

## 代码精讲

### ELF 加载器修复 kernel/mini/elf_loader.cpp

这是整个 tag 最关键的改动——修复了 in-place loading 导致的 ELF 头自毁 bug。

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    if (!parse_elf_header(elf_src)) {
        kprintf("[ELF] ERROR: ELF header validation failed!\n");
        return 0;
    }

    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf_src);
```

函数入口先验证 ELF 头，然后获取头指针。到这里一切正常。

```cpp
    // Step 3: Save header fields before any segment copy.
    const uint64_t  saved_entry     = ehdr->e_entry;
    const uint16_t  saved_phnum     = ehdr->e_phnum;
    const uint64_t  saved_phoff     = ehdr->e_phoff;
    const uint16_t  saved_phentsize = ehdr->e_phentsize;
```

这是修复的核心——在进入加载循环之前，把 ELF 头中的关键字段全部拷贝到栈上的局部变量中。这些字段在后续循环中用来计算 Program Header 的位置和遍历计数，如果它们被覆盖了，循环就会读取垃圾数据。

```cpp
    if (saved_phnum > 16) {
        kprintf("[ELF] ERROR: too many program headers (%u)\n", saved_phnum);
        return 0;
    }
    Elf64_Phdr saved_phdrs[16];
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
            reinterpret_cast<const uint8_t*>(ehdr) + saved_phoff
            + static_cast<uint64_t>(i) * saved_phentsize);
        saved_phdrs[i] = *phdr;
    }
```

更进一步——不仅保存头字段，还把所有 Program Header 深拷贝到栈上的数组中（最多 16 个）。这样后续循环只读 `saved_phdrs[]`，完全不回访 staging buffer 中的 ELF 头。`saved_phnum > 16` 的检查防止栈溢出——我们的内核 ELF 通常只有 3-4 个 PT_LOAD 段，16 个绰绰有余。

```cpp
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const Elf64_Phdr& phdr = saved_phdrs[i];  // 引用栈上的拷贝

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        // ... 边界检查 ...

        uint64_t dest_addr = phdr.p_paddr;
        const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr.p_offset;

        if (phdr.p_filesz > 0) {
            memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
        }

        if (phdr.p_memsz > phdr.p_filesz) {
            uint64_t bss_start = dest_addr + phdr.p_filesz;
            size_t bss_size = static_cast<size_t>(phdr.p_memsz - phdr.p_filesz);
            memset(reinterpret_cast<void*>(bss_start), 0, bss_size);
        }
```

循环体使用 `saved_phdrs` 而非回访 staging buffer。段复制操作用 `memmove` 替代了 `memcpy`——当 `dest_addr`（p_paddr = 0x1000000）与 `elf_src`（staging buffer = 0x1000000）重叠时，`memmove` 能正确处理重叠区域（内部判断前后方向选择拷贝策略），而 `memcpy` 的语义要求不重叠，重叠时是未定义行为。

BSS 清零用 `memset` 处理 `memsz > filesz` 的部分——这是 ELF 加载器的标准职责。

```cpp
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t entry = saved_entry;
    if (entry >= HIGHER_HALF_BASE) {
        entry = entry - HIGHER_HALF_BASE;
    }

    return entry;
```

返回入口点时使用 `saved_entry` 而非回读 ELF 头。如果入口点是 higher-half 虚拟地址（>= 0xFFFFFFFF80000000），减去基址得到物理地址。mini kernel 用这个物理地址做 `jmp *entry` 跳转到大内核。

### CRC32 校验 kernel/mini/lib/crc32.h

CRC32 校验用于验证从磁盘读取的 ELF 数据完整性。

```cpp
// scripts/append_crc32.py 在 ELF 末尾追加 4 字节 CRC32
// C++ 端计算文件主体（排除最后 4 字节）的 CRC32 做比对
```

CRC32 采用查表法实现——256 项的预计算表避免了逐位的 bit 操作，每次处理一个字节只需一次查表和一次异或。这在内核环境下比计算 `zlib.crc32` 更高效。

### QEMU 测试退出机制 cmake/qemu.cmake

```cmake
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)
```

`isa-debug-exit` 设备提供了一个简单的"测试退出"机制——mini kernel 往端口 0xf4 写一个值，QEMU 就会退出。退出码映射是 `(value << 1) | 1`：写 0 → 退出码 1（PASS），写 1 → 退出码 3（FAIL）。

```bash
# scripts/qemu_test_wrapper.sh 把 QEMU 退出码映射为 pass/fail
# exit code 1 → success, exit code 3 → failure
```

Bash wrapper 脚本处理这个映射，让 `make run-kernel-test` 和 `make run-stress-test` 能正确报告成功或失败。

### 压力测试 scripts/generate_large_elf.py

Python 脚本生成一个 1GB 的合成 ELF 文件，包含合法的 ELF64 头和一个 PT_LOAD 段。脚本的参数 `--size 1073741824` 指定了段的大小。

```cmake
add_custom_command(
    OUTPUT ${STRESS_KERNEL_ELF}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/generate_large_elf.py
        --size 1073741824
        --output ${STRESS_KERNEL_ELF}
)

add_custom_target(run-stress-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${STRESS_TEST_IMAGE},format=raw,...
    DEPENDS stress-test-image
)
```

CMake 构建系统定义了完整的 stress test 管线：生成 ELF → 构建 disk image → 运行 QEMU → wrapper 脚本判断退出码。

## 设计决策

### 决策：保存头信息 vs 使用独立 staging buffer

**问题**：如何处理 p_paddr 与 staging buffer 重叠？

**本项目的做法**：加载前保存 ELF 头和 Program Header 到栈上。

**备选方案**：使用两个独立的缓冲区——一个存 ELF 原始数据，一个作为段加载目标。

**为什么不选备选方案**：16MB 的 staging buffer 已经很大了，再分配一个等大的缓冲区对 mini kernel 的内存是巨大浪费。而且 mini kernel 运行在有限的内存环境中（identity mapping，可用物理内存有限）。保存头信息只需要几百字节的栈空间，代价远小于双缓冲。

**如果要扩展/改进**：如果未来内核变得更大（比如 100MB+），可以考虑把 ELF 头单独读到一个小的固定大小缓冲区（512 字节就够了），段数据直接从磁盘流式读取到目标地址，避免 staging buffer 的概念。

### 决策：1GB 压力测试的必要性

**问题**：是否需要 1GB 级别的压力测试？

**本项目的做法**：是的，用 Python 生成 1GB 合成 ELF。

**备选方案**：只测真实的大内核（几十 KB），不做极端测试。

**为什么选当前方案**：真实的 OS 内核将来会增长到几十甚至几百 MB。1GB 压力测试确保整个加载链路（ATA 连续读取、staging buffer 管理、ELF 加载器循环）在大规模数据下仍然正确。而且这种测试暴露的往往是边界条件和整数溢出问题——比如 `filesz` 累加超过 32 位、ATA 扇区计数超过 65536 等。

## 扩展方向

- 实现流式 ELF 加载器（不占 staging buffer，直接从磁盘读到目标地址）（难度 ⭐⭐⭐）
- 添加 ELF section header 解析，支持符号表加载（难度 ⭐⭐）
- 压力测试增加随机数据验证（不只验证入口点，还抽查段内数据）（难度 ⭐⭐）
- 添加多内核并发加载测试（验证内存隔离）（难度 ⭐⭐）
- 实现内核热重载（不重启 QEMU，替换磁盘上的 ELF 并重新加载）（难度 ⭐⭐⭐）

## 参考资料

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式参考
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- Linux: [kernel loader](https://github.com/torvalds/linux/blob/master/arch/x86/boot/compressed/misc.c) — decompressor 中的 ELF 加载
- xv6: [bootmain.c](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 简化的 ELF 加载器
- Intel SDM: Vol.3A §9.1 — 处理器初始化状态
