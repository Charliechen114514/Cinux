# 009-3 Tutorial: 踩到一个大坑 — ELF 加载器自毁 Bug 修复与压力测试

> 标签：ELF loader, in-place loading, memmove, CRC32, stress test, integration test
> 前置：[009-2 I/O + 串口 + kprintf](./009-large-kernel-entry-2.md)

## 前言

这一章的起源是我做集成测试的时候遇到的一个诡异的 bug——mini kernel 加载大内核的 ELF，串口输出了 `PT_LOAD[0]` 的信息后就突然中断了，没有任何异常信息，QEMU 直接退出。如果你和我一样在内核开发中遇到过"输出截断"类的问题，你就会知道这种 bug 有多恶心——你看到的最后一条输出和崩溃点之间可能有几十行代码，要在里面找到真正出问题的地方需要极大的耐心。

结果排查了半天，发现原因出乎意料地简单但隐蔽：ELF 加载器把段数据复制到目标地址时，目标地址恰好就是源缓冲区自身——也就是说它在复制的同时也在破坏自己。这种 in-place loading 的场景在单元测试中根本碰不到，因为手工构造的小 ELF 的 `p_paddr` 不可能和 staging buffer 重叠。只有集成测试——用真实的大内核 ELF 从磁盘加载——才会触发。

## 环境说明

测试环境包括：QEMU 带 `-device isa-debug-exit` 设备（允许 mini kernel 写端口 0xf4 触发退出）、`-no-reboot`（triple fault 后关机而非重启）、`-serial stdio`（串口输出到终端）。大内核 ELF 由 CMake 构建系统生成，经过 `append_crc32.py` 追加校验值后写入磁盘镜像。压力测试使用 `generate_large_elf.py` 生成的 1GB 合成 ELF。

## 第一步——分析 bug：当 memcpy 遇到 in-place loading

先把崩溃现场重新看一下。mini kernel 把大内核 ELF 从磁盘读到 staging buffer（地址 `0x1000000`，即 16MB），然后 `load_elf()` 开始遍历 PT_LOAD 段。大内核的第一个 PT_LOAD 段的 `p_paddr` 是多少呢？没错，也是 `0x1000000`——因为大内核链接在 `KERNEL_LMA = 0x1000000`，链接脚本的 `AT()` 把它映射回这个物理地址。

所以 `memcpy(dest=0x1000000, src=staging_buffer+offset, size=filesz)` 的目标地址就是源缓冲区自身的起始位置。memcpy 在往 0x1000000 写段数据的同时，也覆盖了 staging buffer 里的 ELF 头和 Program Header。第一轮循环结束、进入第二轮迭代时，`get_phdr(ehdr, 1)` 读到的 `ehdr->e_phoff` 已经是垃圾数据了——计算出的指针是野指针，解引用直接 triple fault。

```
崩溃过程:

Staging Buffer @ 0x1000000 (加载前):
┌──────────┬──────────────────────┬─────────────┐
│ ELF 头   │ Program Headers     │ Segment 0   │
│ (64 B)   │ (3x56 B = 168 B)    │ 数据...     │
└──────────┴──────────────────────┴─────────────┘

memcpy(dest=0x1000000, src=0x1000000+0x78, size=0x19F0):

Staging Buffer @ 0x1000000 (加载后):
┌──────────────────────────┬──────────────────┐
│ Segment 0 数据            │ 原始 Segment 0  │
│ ↑ 覆盖了 ELF 头！         │                  │
└──────────────────────────┴──────────────────┘

第二轮迭代: 读取 ehdr->e_phoff → 垃圾值 → 野指针 → #GP → triple fault
```

这种 bug 在操作系统开发中有一个经典名字——"自己吃自己的脚"。Linux 在早期也遇到过类似的问题，它的解决方案是在 decompressor 阶段就做好重定位，确保解压后的内核数据不会覆盖压缩前的头部。xv6 的 bootmain.c 用了一个巧妙的技巧：先读 ELF 头确定所有段的位置，然后从磁盘重新读取段数据到目标地址——这样源和目标永远不会重叠，因为源是磁盘而非内存中的缓冲区。

## 第二步——修复：保存头信息 + memmove

修复方案分两部分：第一，在进入加载循环之前把所有需要的元数据保存到栈上；第二，把 `memcpy` 换成 `memmove`。

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    if (!parse_elf_header(elf_src)) { return 0; }

    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf_src);

    // ★ 修复：在段复制之前保存所有头信息
    const uint64_t  saved_entry     = ehdr->e_entry;
    const uint16_t  saved_phnum     = ehdr->e_phnum;
    const uint64_t  saved_phoff     = ehdr->e_phoff;
    const uint16_t  saved_phentsize = ehdr->e_phentsize;

    // ★ 深拷贝所有 Program Header 到栈上
    if (saved_phnum > 16) { return 0; }
    Elf64_Phdr saved_phdrs[16];
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
            reinterpret_cast<const uint8_t*>(ehdr) + saved_phoff
            + static_cast<uint64_t>(i) * saved_phentsize);
        saved_phdrs[i] = *phdr;
    }
```

进入循环前把 ELF 头中的关键字段（`e_entry`、`e_phnum`、`e_phoff`、`e_phentsize`）拷贝到局部变量，再把所有 Program Header 深拷贝到栈上的数组。后续循环只读 `saved_phdrs[]`，完全不回访 staging buffer。

```cpp
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const Elf64_Phdr& phdr = saved_phdrs[i];  // 引用栈上的拷贝

        if (phdr.p_type != PT_LOAD) continue;

        uint64_t dest_addr = phdr.p_paddr;
        const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr.p_offset;

        if (phdr.p_filesz > 0) {
            memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
        }

        if (phdr.p_memsz > phdr.p_filesz) {
            memset(reinterpret_cast<void*>(dest_addr + phdr.p_filesz), 0,
                   phdr.p_memsz - phdr.p_filesz);
        }
    }

    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t entry = saved_entry;
    if (entry >= HIGHER_HALF_BASE) entry -= HIGHER_HALF_BASE;
    return entry;
```

`memmove` 替代 `memcpy`——C 标准规定 `memmove` 正确处理重叠区域，内部判断前后方向选择合适的拷贝策略。虽然我们手写的 `memcpy` 恰好是正向逐字节拷贝（`dest < src` 时不会出错），但依赖实现细节是未定义行为，不能靠运气编程。返回入口点时用 `saved_entry` 而非回读 ELF 头——此时 ELF 头可能已经被段数据覆盖了。

SerenityOS 的 Prekernel 也面临类似的挑战——它需要把内核从 GRUB 加载的位置搬运到最终的虚拟地址。但 SerenityOS 的做法更复杂：先建立页表映射，再通过虚拟地址访问目标位置，天然避免了源和目标的物理地址重叠问题。xv6 的 bootmain.c 最简单——从磁盘重新读取段数据，源是磁盘而非内存缓冲区，永远不会重叠。

## 第三步——CRC32 校验：确保磁盘读取正确

修复了 ELF 头自毁之后，我们还需要确保从磁盘读取的数据本身就是正确的——毕竟 ATA PIO 读取偶尔会出错（虽然 QEMU 里几乎不会）。

做法是在构建时用 Python 脚本 `append_crc32.py` 计算大内核 ELF 的 CRC32 校验值（4 字节），追加到文件末尾。mini kernel 加载时读取文件末尾的校验值，重新计算文件主体的 CRC32，比对一致才继续。CRC32 用查表法实现（`kernel/mini/lib/crc32.h`）——256 项的预计算表，每次处理一个字节只需一次查表和一次异或，效率很高。

## 第四步——压力测试：1GB 内核加载

为了验证整个加载链路在极端情况下的鲁棒性，我们用 `generate_large_elf.py` 生成一个 1GB 的合成 ELF 文件。这个 ELF 有合法的 ELF64 头和一个 PT_LOAD 段（`p_filesz = p_memsz = 1GB`），段内填充零字节。

1GB 意味着 mini kernel 需要从磁盘读取大约 2000 个扇区——这对 ATA 驱动的连续读取能力和 ELF 加载器的内存管理都是考验。QEMU 配置了 `-m 8G` 确保虚拟机有足够内存。

```cmake
add_custom_target(run-stress-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${STRESS_TEST_IMAGE},format=raw,index=0,media=disk,cache=unsafe
    DEPENDS stress-test-image
)
```

`isa-debug-exit` 设备让 mini kernel 可以通过写端口 0xf4 触发 QEMU 退出。退出码映射是 `(value << 1) | 1`：写 0 → 退出码 1（PASS），写 1 → 退出码 3（FAIL）。bash wrapper 脚本 `qemu_test_wrapper.sh` 处理这个映射。

你可能会问：1GB 的压力测试会不会太过了？其实不会。Linux 的 decompressor 就是要处理几百 MB 的内核镜像。xv6 虽然小，但它的 bootmain.c 也没有做大小限制——理论上也能加载大文件（虽然 xv6 的内核本身很小）。SerenityOS 的内核镜像在 strip 后也有几十 MB。教学 OS 提前做好大规模测试，是在为将来打基础。

## 收尾

到这里 tag 009 的所有工作都完成了。我们修复了 ELF 加载器最隐蔽的 bug（in-place loading 自毁），添加了 CRC32 校验确保数据完整性，用 1GB 压力测试验证了加载链路的鲁棒性。整个 tag 的代码量超过一万行（10,408 additions），涉及大内核启动、I/O 端口、串口驱动、kprintf、ELF 加载器修复、构建系统扩展、测试框架——从零开始搭起了大内核的基础设施。

下一个 tag（010）要在大内核里设置 GDT 和 IDT——让内核能处理异常和中断。有了本章的 kprintf 输出能力，到时候调试中断处理就会轻松很多了。

## 参考资料

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式和 PT_LOAD 加载
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- Linux: [decompressor misc.c](https://github.com/torvalds/linux/blob/master/arch/x86/boot/compressed/misc.c) — 内核解压和重定位
- xv6: [bootmain.c](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 从磁盘重新读取段数据的方案
- SerenityOS: [Prekernel](https://github.com/SerenityOS/serenity/blob/master/Kernel/Prekernel/Prekernel.cpp) — 页表映射搬运内核
- Intel SDM: Vol.3A §9.1 — 处理器初始化状态
