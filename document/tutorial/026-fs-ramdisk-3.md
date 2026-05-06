# Ramdisk 驱动实现——遍历与解析

> 标签：ramdisk, ustar, mount, 链接器符号, 内核测试, host 测试
> 前置：[026-fs-ramdisk-2](026-fs-ramdisk-2.md)

## 前言

前两篇我们把"图纸"（ustar 
格式定义）和"桥梁"（二进制嵌入管道）都准备好了。现在到了最激动人心的部分——写一
个 Ramdisk 驱动，在内核启动时遍历嵌入的 initrd 
归档，把每个文件的名字和大小打印到串口上。

说白了，Ramdisk 驱动的核心逻辑就是一个 while 循环：每次从当前位置读一个 512 
字节的头部，检查 name 字段是否为空（空表示归档结束），验证 magic 字段是否为 
"ustar"，解析 size 字段得到文件大小，根据 typeflag 
判断类型，打印信息，然后跳到下一个头部。循环直到归档结束或遇到非法数据。

这个逻辑本身不难，但它涉及到几个 OS 
开发中的基本功：通过链接器符号访问嵌入式数据、安全地输出非 null 
终止的字符串、正确的块对齐跳转计算。我们来一步步拆解。

## 环境说明

内核代码运行在 higher-half 地址空间，initrd 数据通过链接器符号 
`_binary_initrd_start/end` 访问。串口输出通过 `kprintf` 
函数。内核中禁用了标准库，所有内存操作直接使用指针算术和 `reinterpret_cast`。

## 从链接器符号到内存指针

Ramdisk 类的 `mount()` 方法做的第一件事是从链接器符号获取 initrd 数据的边界：

```cpp
extern "C" {
extern const uint8_t _binary_initrd_start[];
extern const uint8_t _binary_initrd_end[];
}
```

这段声明看起来有点奇怪——为什么是数组而不是指针？原因是链接器符号代表的是内存中
的地址。在 C/C++ 中，数组名天然就是首元素的地址，所以 `_binary_initrd_start` 
的值就是 initrd 数据的第一个字节所在的地址。如果声明为 `extern const uint8_t* 
_binary_initrd_start`，链接器会把符号的**地址**当作指针值来用，你拿到的是一个
指向错误位置的指针。

`extern "C"` 也是必须的——这些符号由 objcopy 生成，没有 C++ 的 name 
mangling。如果不加 `extern "C"`，C++ 编译器会把符号名改成类似 
`_Z20_binary_initrd_start` 的东西，链接时找不到。

mount 中获取数据的方式非常干净：

```cpp
base_ = _binary_initrd_start;
size_ = static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start);
```

两个指针相减直接得到字节数，不需要任何额外的计算。

## 安全的文件名输出

ustar 规范说 name 字段应该以 null 终止，但实际上当文件名恰好 100 
字节长的时候（99 个字符 + 1 个 null），这没问题。但如果某个有 bug 
的工具生成了 100 个非 null 字符的 name，用 `kprintf("%s", hdr->name)` 
就会越界读取，直到碰到一个 null 
字节为止——在内核环境中，这可能导致读取到随机的内存内容甚至触发 page fault。

所以我们需要一个有界输出函数：

```cpp
void print_bounded(const char* str, uint32_t max_len) {
    for (uint32_t i = 0; i < max_len; ++i) {
        if (str[i] == '\0') {
            break;
        }
        cinux::lib::kprintf("%c", str[i]);
    }
}
```

逐字符打印，最多 max_len（100）个字符，遇 null 
停止。虽然效率不是最优（每个字符一次 kprintf 调用），但 initrd 
中的文件数量很少，性能完全不是问题。在内核开发中，"安全"永远比"快"重要。

## mount 核心循环

现在来看 Ramdisk::mount() 的主循环。这段代码清晰地体现了 ustar 
遍历的标准模式：

```cpp
uint32_t entry_count = 0;
uint64_t offset = 0;

while (offset + sizeof(UstarHeader) <= size_) {
    auto* hdr = reinterpret_cast<const UstarHeader*>(base_ + offset);
```

while 条件 `offset + sizeof(UstarHeader) <= size_` 
确保剩余空间至少能容纳一个完整的 512 字节头部。注意这里用的是 `<=` 而不是 
`<`——`sizeof(UstarHeader)` 编译期等于 512，如果剩余空间恰好是 512 
字节，我们应该尝试解析最后一个头部。`reinterpret_cast` 
把原始字节重解释为头部结构体——这之所以安全，是因为 `UstarHeader` 是 packed 
struct，布局和磁盘数据精确对应。

```cpp
    if (hdr->name[0] == '\0') {
        break;
    }
    if (!is_valid_ustar(hdr)) {
        cinux::lib::kprintf("[RAMDISK] Invalid ustar magic at offset %u, stopping.\n", 
offset);
        break;
    }
```

两种停止条件。name[0] 为 null 意味着这是一个全零块——ustar 
规范用两个连续的全零块标记归档结束，我们只需要检测第一个就够了（第二个不会被 
while 条件放过，因为剩余空间不足一个头部）。magic 
不匹配则说明数据损坏或者我们跳到了归档之外的内存区域——打印偏移量帮助调试。

```cpp
    uint64_t file_size = octal_to_uint(hdr->size, sizeof(hdr->size));

    char type = hdr->typeflag;
    if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
        cinux::lib::kprintf("[RAMDISK]   FILE: ");
        print_bounded(hdr->name, RAMDISK_NAME_MAX);
        cinux::lib::kprintf("  (%u bytes)\n", file_size);
        ++entry_count;
    } else if (type == UstarType::DIRECTORY) {
        cinux::lib::kprintf("[RAMDISK]   DIR:  ");
        print_bounded(hdr->name, RAMDISK_NAME_MAX);
        cinux::lib::kprintf("\n");
    }
```

解析文件大小（八进制字符串转整数），根据 typeflag 
分类处理。CONTIGUOUS（'7'）和 REGULAR（'0'）同等对待——在 POSIX 
规范中它们是等价的。entry_count 
只计文件不计目录，因为返回值的语义是"找到了多少个文件"。

```cpp
    uint32_t blocks = data_blocks(file_size);
    offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * 
USTAR_BLOCK_SIZE;
}
```

最后一步——跳转到下一个条目。头部占 512 字节（一个块），数据占 blocks 个 512 
字节块。blocks 的计算是向上取整：size 为 0 返回 
0（目录和空文件没有数据块），size 为 1-512 返回 1，size 为 513-1024 返回 
2，以此类推。这个计算的准确性是整个遍历正确性的关键——如果跳多了会跳过条目，跳
少了会误把文件数据当成头部来解析，两种情况都会导致后续所有数据错位。

## 内核集成

Ramdisk 被集成为内核启动序列的第 22 步（紧接在 AHCI 初始化之后）：

```cpp
cinux::lib::kprintf("[BIG] ===== Milestone 026: Ramdisk (initrd) =====\n");
cinux::fs::Ramdisk ramdisk;
ramdisk.mount();
```

就这么简单——创建对象、调用 mount、串口自动打印结果。Ramdisk 
对象是局部变量，mount 后不需要做任何额外操作。这种"即用即走"的设计使得 
ramdisk 初始化几乎不影响内核启动流程的复杂度。

## 测试策略

tag 026 的测试体系值得一提，因为它展示了一种常见的内核测试模式：host 
端纯逻辑测试 + QEMU 内核集成测试的双层设计。

内核测试（`kernel/test/test_ramdisk.cpp`）在 QEMU 中运行，直接调用真实的 
`Ramdisk::mount()`，验证真实的 initrd 
归档能被正确解析。这些测试验证的是"端到端"的正确性——从链接器符号到文件计数，整
个链路都在真实环境中跑通。

Host 端测试（`test/unit/test_ramdisk.cpp`，867 
行）更全面也更细致。由于内核代码依赖 kprintf 和链接器符号，在 host 
环境中无法直接运行，所以测试文件镜像了所有纯逻辑函数（`octal_to_uint`、`is_val
id_ustar`、`data_blocks`、`mount_archive`），然后用手工构造的 ustar 
归档测试各种场景：空归档、单文件、多文件、混合文件和目录、损坏的 
magic、非对齐的文件大小、各种 typeflag 等等。

这种"镜像纯逻辑"的方式虽然不完美（测试的是镜像而不是真实代码），但在内核开发中
是务实的权衡——纯逻辑函数足够简单，镜像实现几乎不会引入差异，测试的主要价值在于
覆盖各种边界情况和验证算法正确性。

## 其他 OS 的 initrd 集成方式对比

**xv6** 的 ramdisk 集成方式和 Cinux 完全不同。xv6 把 
`fs.img`（文件系统镜像）通过 QEMU 的 `-rdinit` 
选项加载到固定物理地址，内核通过 `memlayout.h` 中硬编码的 `RAMDISK` 
常量访问。`ramdiskrw()` 
函数提供块级读写，上层的文件系统代码（`fs.c`）负责从这些块中解析文件和目录。

这种设计把"数据怎么来"和"数据怎么用"完全分离——ramdisk 
驱动不关心文件系统格式，文件系统代码不关心数据来源。好处是换一个存储后端（比如
从 ramdisk 
换成真正的磁盘驱动）不需要改文件系统代码。坏处是整个栈更复杂——你需要先实现块设
备驱动，再实现文件系统，才能读取第一个文件。

Cinux 的选择是先做"扁平面"的 initrd 解析——直接在 ramdisk 驱动里解析 ustar 
格式，跳过块设备层。这在 tag 026 的阶段是合理的：我们还没有 VFS（那是 tag 027 
的事），先验证能正确解析归档比搭建完整的分层架构更重要。等 VFS 
就绪后，ramdisk 可以被封装成一个块设备，和磁盘驱动享受同样的抽象。

**Linux** 的 initrd 集成更加成熟。内核支持多种 initrd 格式（cpio、ext2 
镜像、initramfs），有专门的解压和挂载基础设施，还支持在 initrd 
中加载内核模块。Linux 的 `init/do_mounts_initrd.c` 和 `init/initramfs.c` 
负责整个 initrd 的生命周期管理——从解析到挂载到最终的 pivot_root 
切换。这些代码加起来有上千行，体现了生产级 OS 对 initrd 的重视程度。

## 收尾

到这里，tag 026 的所有工作就完成了。我们定义了 ustar 
格式映射、搭建了二进制嵌入管道、实现了 Ramdisk 
驱动、集成到了内核启动流程、还搭了完整的双层测试体系。启动内核后，串口会列出 
initrd 中的所有文件——这是我们的内核第一次"看到"文件。

下一章（tag 027 VFS）会在这基础上继续——把 ramdisk 
从一个简单的枚举器升级为一个真正的文件系统后端，提供 open/read/write/close 
接口。到时候，用户态的 shell 就能从 initrd 中读取文件了。可以说，这一章的 
ramdisk 是通往文件系统的第一块基石。

## 参考资料

- OSDev Wiki: [Initrd](https://wiki.osdev.org/Initrd) — initrd 的各种使用模式
- OSDev Wiki: [Tar](https://wiki.osdev.org/Tar) — ustar 遍历的标准模式
- xv6 ramdisk: [kernel/ramdisk.c](https://github.com/mit-pdos/xv6-riscv) — 
xv6 的块设备级 ramdisk
- Linux initrd: [kernel.org](https://docs.kernel.org/admin-guide/initrd.html) 
— Linux initrd 的完整生命周期
- Linux initramfs: 
[kernel.org](https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html) 
— Linux initramfs 机制
