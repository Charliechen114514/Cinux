# 009-3 Tutorial: 踩到一个大坑 — ELF 加载器自毁 Bug 修复与压力测试

> 标签：ELF loader, in-place loading, memmove, CRC32, stress test, integration test, isa-debug-exit
> 前置：[009-2 I/O + 串口 + kprintf](./009-large-kernel-entry-2.md)

## 前言

这一章的起源是我做集成测试的时候遇到的一个诡异的 bug——mini kernel 加载大内核的 ELF，串口输出了 `PT_LOAD[0]` 的信息后就突然中断了，没有任何异常信息，QEMU 直接退出。如果你和我一样在内核开发中遇到过"输出截断"类的问题，你就会知道这种 bug 有多恶心——你看到的最后一条输出和崩溃点之间可能有几十行代码，要在里面找到真正出问题的地方需要极大的耐心。

结果排查了半天，发现原因出乎意料地简单但隐蔽：ELF 加载器把段数据复制到目标地址时，目标地址恰好就是源缓冲区自身——也就是说它在复制的同时也在破坏自己。这种 in-place loading 的场景在单元测试中根本碰不到，因为手工构造的小 ELF 的 `p_paddr` 不可能和 staging buffer 重叠。只有集成测试——用真实的大内核 ELF 从磁盘加载——才会触发。

这一章的三个主题（bug 修复、CRC32 校验、压力测试）构成了 tag 009 的收尾工作。bug 修复让加载器在真实场景下正确工作，CRC32 校验确保数据从磁盘到内存的完整性，压力测试验证整个系统在极端条件下的鲁棒性。

这个故事也给了我们一个重要教训：单元测试和集成测试是互补的，缺一不可。单元测试验证每个组件的独立正确性，集成测试验证组件组合后在真实环境下的行为。in-place loading 的 bug 只有在组件组合（ATA 驱动 + ELF 加载器 + 大内核链接脚本）产生特定的地址布局时才会出现，这正是集成测试的价值所在。

让我们从 bug 的复现开始，逐步揭开这个问题的根因。

## 环境说明

测试环境包括：QEMU 带 `-device isa-debug-exit,iobase=0xf4,iosize=0x04` 设备（允许 mini kernel 写端口 0xf4 触发退出）、`-no-reboot`（triple fault 后关机而非重启）、`-serial stdio`（串口输出到终端）、`-m 8G -accel kvm`（8GB 内存 + KVM 加速）。`isa-debug-exit` 设备的退出码映射比较特殊：`exit_code = (value << 1) | 1`，也就是说 mini kernel 往 0xf4 端口写 0 时 QEMU 以退出码 1 退出（PASS），写 1 时以退出码 3 退出（FAIL）。一个 bash wrapper 脚本 (`qemu_test_wrapper.sh`) 处理这个映射，让 `make run-kernel-test` 能正确报告成功或失败。

大内核 ELF 由 CMake 构建系统生成，经过 `append_crc32.py` 追加校验值后写入磁盘镜像。压力测试使用 `generate_large_elf.py` 生成的 1GB 合成 ELF。

## 第一步——分析 bug：当 memcpy 遇到 in-place loading

先把崩溃现场重新看一下。mini kernel 把大内核 ELF 从磁盘读到 staging buffer（地址 `0x1000000`，即 16MB），然后 `load_elf()` 开始遍历 PT_LOAD 段。大内核的第一个 PT_LOAD 段的 `p_paddr` 是多少呢？没错，也是 `0x1000000`——因为大内核链接在 `KERNEL_LMA = 0x1000000`，链接脚本的 `AT()` 把它映射回这个物理地址。

这个巧合不是偶然的——staging buffer 的地址和 ELF 段的物理加载地址都是从 `BIG_KERNEL_LOAD_ADDR` 这个常量派生出来的。单元测试里手工构造的 ELF 会把 `p_paddr` 设成栈上的地址（完全不同的值），所以永远不会和 staging buffer 重叠。只有真实的大内核 ELF 才会产生这种重叠。

所以 `memcpy(dest=0x1000000, src=staging_buffer+offset, size=filesz)` 的目标地址就是源缓冲区自身的起始位置。memcpy 在往 0x1000000 写段数据的同时，也覆盖了 staging buffer 里的 ELF 头和 Program Header。第一轮循环结束、进入第二轮迭代时，`get_phdr(ehdr, 1)` 读到的 `ehdr->e_phoff` 已经是垃圾数据了——计算出的指针是野指针，解引用直接 triple fault。

值得注意的是崩溃发生的位置——在两条 `kprintf` 之间。上一条输出 `PT_LOAD[0]` 的诊断信息正常打印了，下一条 `Loaded segment 0` 的消息却永远不会出现。这种"最后一条输出"式的调试，是内核开发中最常用的定位手法：通过在可疑位置前后各加一条 `kprintf`，看哪一条没输出就知道崩溃发生在这两条输出之间。

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

要理解为什么这个 bug 在 QEMU 的 `-no-reboot` 配置下表现为直接关机而不是重启，需要了解 x86 的 triple fault 机制。当 CPU 遇到一个异常（比如 `#PF`，Page Fault），它会尝试通过 IDT 调用对应的异常处理程序。如果这个处理过程本身又触发了异常（比如处理程序代码所在的页不在内存中，或者 IDT 本身的描述符无效），CPU 就会触发 Double Fault（`#DF`）。如果 Double Fault 的处理也失败了，CPU 就进入了 Triple Fault 状态——这是一个不可恢复的错误，CPU 会自动重置。`-no-reboot` 参数告诉 QEMU 在重置时直接关机而不是重启虚拟机，这样我们在串口上看到的就是输出截断后 QEMU 退出，而不是无限重启循环。

## 第二步——修复：保存头信息 + memmove

修复方案分两部分：第一，在进入加载循环之前把所有需要的元数据保存到栈上；第二，把 `memcpy` 换成 `memmove`。这两部分缺一不可——只保存头信息但不换 memmove，段复制仍然是未定义行为；只换 memmove 但不保存头信息，虽然复制本身不会破坏数据了，但第一轮复制后 staging buffer 里的 ELF 头已经被覆盖，后续迭代读到的仍然是垃圾数据。

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

进入循环前把 ELF 头中的关键字段（`e_entry`、`e_phnum`、`e_phoff`、`e_phentsize`）拷贝到局部变量，再把所有 Program Header 深拷贝到栈上的数组。后续循环只读 `saved_phdrs[]`，完全不回访 staging buffer。这四字段的选取是经过仔细考量的——它们是后续循环和入口点计算中唯一需要的头信息，缺一个都不行：`e_phnum` 控制循环次数，`e_phoff` 和 `e_phentsize` 定位 Program Header，`e_entry` 是最终的返回值。

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

`memmove` 替代 `memcpy`——C 标准规定 `memmove` 正确处理重叠区域，内部判断前后方向选择合适的拷贝策略。虽然我们手写的 `memcpy` 恰好是正向逐字节拷贝（`dest < src` 时不会出错），但依赖实现细节是未定义行为，不能靠运气编程。编译器有权假设 `memcpy` 的源和目标不重叠，据此做更激进的优化（比如 SIMD 向量化拷贝），一旦重叠就会出问题。返回入口点时用 `saved_entry` 而非回读 ELF 头——此时 ELF 头可能已经被段数据覆盖了。

`saved_phdrs[16]` 这个上限值的选择也有讲究。ELF 规范没有对 Program Header 数量设上限，但实际操作系统中一个可执行文件的 PT_LOAD 段通常只有 3-5 个。16 是一个安全的上限——大到足以容纳任何合理的内核 ELF，小到不会浪费太多栈空间（每个 `Elf64_Phdr` 是 56 字节，16 个也就 896 字节）。如果将来有人真的弄出了超过 16 个 Program Header 的 ELF，`saved_phnum > 16` 的检查会让加载器优雅地报错退出，而不是栈溢出后 triple fault。

SerenityOS 的 Prekernel 也面临类似的挑战——它需要把内核从 GRUB 加载的位置搬运到最终的虚拟地址。但 SerenityOS 的做法更复杂：先建立页表映射，再通过虚拟地址访问目标位置，天然避免了源和目标的物理地址重叠问题。xv6 的 bootmain.c 最简单——从磁盘重新读取段数据，源是磁盘而非内存缓冲区，永远不会重叠。

除了核心的头信息保存和 memmove 替换，我们还修复了集成测试中的一个相关问题：`test_entry_address` 测试原本会二次调用 `load_elf()` 来获取入口地址，但此时 staging buffer 已经被段数据覆盖了——ELF 魔数不复存在，`parse_elf_header()` 必定失败。修复方式是使用一个文件局部的共享变量 `g_loaded_entry` 在测试用例之间传递数据。这个修改虽然简单，但它暴露了一个重要的设计原则：ELF 加载是破坏性操作，加载完成后 staging buffer 里的元数据不再可靠，所有需要的信息必须在加载过程中保存下来。

## 第三步——CRC32 校验：确保磁盘读取正确

修复了 ELF 头自毁之后，我们还需要确保从磁盘读取的数据本身就是正确的——毕竟 ATA PIO 读取偶尔会出错（虽然 QEMU 里几乎不会）。在真实硬件上，磁盘读取错误的后果是灾难性的：错误的段数据会被当作机器码执行，行为完全不可预测，而且通常不会有任何有用的报错信息。

做法是在构建时用 Python 脚本 `append_crc32.py` 计算大内核 ELF 的 CRC32 校验值（4 字节），追加到文件末尾。mini kernel 加载时读取文件末尾的校验值，重新计算文件主体的 CRC32，比对一致才继续。CRC32 用查表法实现（`kernel/mini/lib/crc32.h`）——256 项的预计算表，每次处理一个字节只需一次查表和一次异或，效率很高。

CRC32 的多项式是 `0xEDB88320`（reflected 形式），这是 CRC-32/ISO-HDLC 的标准多项式，和 Python 的 `zlib.crc32()` 完全一致——这保证了 Python 端构建时计算的校验值和 C++ 端运行时计算的校验值能匹配上。一个小细节是校验值的范围：Python 端计算的是整个 ELF 文件（不含追加的 4 字节），C++ 端也只计算文件主体部分，两端的数据范围必须严格一致。

CRC32 校验值追加在 ELF 文件末尾后，这些字节不属于任何 ELF section——ELF 的 section header table 仍然指向原始位置。这意味着 `readelf` 或 `objdump` 可能会对修改后的文件发出警告，但这是预期行为，不影响 ELF 加载器的正常工作。

构建流水线中还有一个辅助脚本 `scripts/check_memory_layout.py`，它在构建时自动解析大内核 ELF 的 program header，检查以下约束：所有 PT_LOAD 段的 `p_paddr` 应该在物理地址的合理范围内（不低于 `KERNEL_LMA`，不超过可用内存上限）；相邻段之间不应该有太大的间隙（可能表示链接脚本配置错误）；段的 `p_memsz` 和 `p_filesz` 的关系应该合理（`memsz >= filesz`，差值代表 BSS）。如果任何检查失败，脚本以非零退出码退出，阻止构建继续——这是"fail fast"策略在构建系统中的体现。

## 第四步——压力测试：1GB 内核加载

为了验证整个加载链路在极端情况下的鲁棒性，我们用 `generate_large_elf.py` 生成一个 1GB 的合成 ELF 文件。这个 ELF 有合法的 ELF64 头和一个 PT_LOAD 段（`p_filesz = p_memsz = 1GB`），段内填充零字节。

为什么需要这么极端的测试？因为真实的 OS 内核将来会增长到几十甚至几百 MB。1GB 的压力测试确保整个加载链路（ATA 连续读取、staging buffer 管理、ELF 加载器循环）在大规模数据下仍然正确。这种测试暴露的往往是边界条件和整数溢出问题——比如 `filesz` 累加超过 32 位、ATA 扇区计数超过 65536 等。提前在极端条件下验证，比将来出了问题再排查要省事得多。

1GB 意味着 mini kernel 需要从磁盘读取大约 2097152 个扇区（1GB / 512 字节每扇区）——这对 ATA 驱动的连续读取能力和 ELF 加载器的内存管理都是考验。QEMU 配置了 `-m 8G` 确保虚拟机有足够内存来容纳这个巨型内核。

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

压力测试的成功标准很简单：mini kernel 加载完 1GB 的 ELF 后，入口点地址正确，然后往 QEMU 的 `isa-debug-exit` 设备写 0 表示 PASS，QEMU 以退出码 1 退出。如果加载过程中任何一个步骤出错（ELF 头校验失败、段边界越界、CRC 不匹配），mini kernel 写 1 表示 FAIL，QEMU 以退出码 3 退出。bash wrapper 脚本把这两个退出码映射为 `make` 的成功/失败，让 CI 能自动化运行压力测试。

CMake 构建系统中定义了完整的 stress test 管线：`stress-kernel-elf` target 调用 Python 脚本生成 1GB ELF，`stress-test-image` target 把 mini kernel test 和这个巨型 ELF 写入磁盘镜像，`run-stress-test` target 通过 wrapper 脚本启动 QEMU 并判断退出码。`generate_large_elf.py` 脚本生成的 ELF 包含正确的 ELF64 magic（`\x7fELF`）、machine type（EM_X86_64 = 62）、一个 PT_LOAD 段——结构合法但内容为零填充。重点是验证加载链路的正确性，而非段数据的含义。

值得注意的是压力测试的磁盘镜像使用了 `-drive ... cache=unsafe` 参数——这告诉 QEMU 不要对磁盘写入做同步刷新，加快测试速度。在生产运行中我们不会用这个参数，但对于一次性的测试镜像来说，安全性不是关注点，速度才是。

压力测试运行时间会比较长（1GB 的磁盘读取 + 内存清零），通常需要几十秒到几分钟。如果超时或失败，首先检查磁盘空间是否充足——1GB 的 ELF 加上磁盘镜像的开销，构建目录至少需要 2-3GB 的可用空间。

## 收尾

到这里 tag 009 的所有工作都完成了。我们修复了 ELF 加载器最隐蔽的 bug（in-place loading 自毁），添加了 CRC32 校验确保数据完整性，用 1GB 压力测试验证了加载链路的鲁棒性。整个 tag 的代码量超过一万行（10,408 additions），涉及大内核启动、I/O 端口、串口驱动、kprintf、ELF 加载器修复、构建系统扩展、测试框架——从零开始搭起了大内核的基础设施。

回过头看，这个 tag 最大的收获不是某个具体功能的实现，而是对"测试策略"的认识：单元测试验证组件独立性，集成测试验证组件组合的正确性，压力测试验证极端条件下的鲁棒性。in-place loading 的 bug 是一堂生动的课——它教会我们，有些问题只有当所有组件在真实环境下协同工作时才会浮现，任何单一组件的测试都无法发现。

下一个 tag（010）要在大内核里设置 GDT 和 IDT——让内核能处理异常和中断。有了本章的 kprintf 输出能力和经过验证的 ELF 加载链路，到时候调试中断处理就会轻松很多了。

## 构建与运行

```bash
# 完整构建（包括 stress test ELF 生成）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 运行集成测试
cd build && make run-kernel-test
# 预期：=== ALL TESTS PASSED ===，QEMU 退出码 1

# 运行压力测试
cd build && make run-stress-test
# 预期：stress test 通过，QEMU 正常退出

# 查看 debug console 输出（0xE9 端口）
cat build/debug.log
# 预期：空的（没有 'V' 或 'S' 字符，说明没有触发纯虚函数或栈保护失败）
```

构建系统还提供了其他有用的 target：`make stress-kernel-elf` 单独生成 1GB ELF（不运行测试），`make stress-test-image` 单独构建压力测试磁盘镜像。这些 target 的拆分让你可以分步调试构建流程中的问题。

## 参考资料

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式和 PT_LOAD 加载
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- OSDev Wiki: [CRC32](https://wiki.osdev.org/CRC32) — CRC32 算法和查表法实现
- Linux: [decompressor misc.c](https://github.com/torvalds/linux/blob/master/arch/x86/boot/compressed/misc.c) — 内核解压和重定位
- xv6: [bootmain.c](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 从磁盘重新读取段数据的方案
- SerenityOS: [Prekernel](https://github.com/SerenityOS/serenity/blob/master/Kernel/Prekernel/init.cpp) — 页表映射搬运内核
- Intel SDM: Vol.3A §9.1 — 处理器初始化状态
- Intel SDM: Vol.3A §6.15 — Triple Fault 和 Double Fault 的处理流程
- QEMU: [isa-debug-exit](https://github.com/qemu/qemu/blob/master/hw/misc/debugexit.c) — QEMU 测试退出设备文档
