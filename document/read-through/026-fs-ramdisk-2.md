# 026-fs-ramdisk-2: Ramdisk 类实现

## 概览

本文是 tag 026 代码通读的第二篇，聚焦在 `kernel/fs/ramdisk.hpp` 和 
`kernel/fs/ramdisk.cpp`——Ramdisk 驱动的核心实现。上一篇我们看了 ustar 
格式的定义，现在来看 Cinux 是怎么利用这些定义来解析嵌入的 initrd 归档的。

关键设计决策一览：Ramdisk 类采用"构造-后挂载"模式（先构造空对象，再调用 mount 
解析）；内部辅助函数放在匿名命名空间中限制可见性；文件名打印使用有界输出函数防
止越界；链接器符号通过 `extern "C"` 数组声明来获取。

## 架构图

```
Ramdisk 类
  ├─ 私有: base_ (const uint8_t*), size_ (uint64_t)
  ├─ 公共: mount(), base(), total_size()
  │
  ├─ mount() 调用链:
  │   ├─ 链接器符号 → base_, size_
  │   ├─ while 循环遍历:
  │   │   ├─ is_valid_ustar(hdr)    [匿名命名空间]
  │   │   ├─ octal_to_uint(hdr->size, 12)
  │   │   ├─ print_bounded(hdr->name, 100)  [匿名命名空间]
  │   │   └─ data_blocks(file_size)  [匿名命名空间]
  │   └─ 返回文件计数
  │
  └─ 依赖:
      ├─ ramdisk_config.hpp (UstarHeader, UstarType, octal_to_uint)
      └─ lib/kprintf.hpp (串口输出)
```

## 代码精讲

### 链接器符号声明

```cpp
extern "C" {
extern const uint8_t _binary_initrd_start[];
extern const uint8_t _binary_initrd_end[];
}
```

这段代码声明了由 `embed_binary.sh` 脚本生成的两个链接器符号。`extern "C"` 
是必须的——这些符号是 objcopy 生成的 C 风格符号，C++ 的 name mangling 
会改变符号名导致链接失败。声明为数组类型（`[]`）而不是指针类型是因为链接器符号
本质上代表一个地址，数组名在 C/C++ 中天然代表其首元素的地址，所以 `&` 和 `*` 
操作都能正确工作。

计算归档大小的方式简洁而安全：`size_ = 
static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start)`。两个 
`const uint8_t*` 指针相减得到元素个数（字节数），类型转换确保是 64 
位无符号整数。

### RamdiskEntry 结构体

```cpp
struct RamdiskEntry {
    const char* name;     ///< Pointer into the ustar header name field
    uint64_t    size;     ///< File size in bytes (parsed from octal)
    const void* data;     ///< Pointer to the file data within the archive
};
```

这个结构体目前虽然定义了，但在 tag 026 中没有被实际使用——mount 
只做打印不做记录。它为 tag 027（VFS）预留了接口：当 VFS 
需要按文件名查找时，可以遍历 RamdiskEntry 数组获取文件信息和数据指针。name 
指针直接指向归档内部，不拷贝字符串，这在内核环境中是正确的选择——避免动态分配内
存，也避免了字符串拷贝的开销。

### Ramdisk 类接口

```cpp
class Ramdisk {
public:
    uint32_t mount();
    const void* base() const;
    uint64_t total_size() const;

private:
    const uint8_t* base_{};
    uint64_t size_{};
};
```

类设计非常精简：两个私有成员用 C++11 的类内初始化（值初始化为 
0/nullptr），不需要手写构造函数。`mount()` 
是核心方法，返回找到的文件条目数。`base()` 和 `total_size()` 
是只读访问器，方便外部代码查询 ramdisk 的元信息。

这种"构造后调用 
mount"的模式比在构造函数中挂载更好——如果挂载失败，调用者可以检查返回值而不是依
赖异常（内核中禁用了异常）。

### octal_to_uint 实现

```cpp
uint64_t octal_to_uint(const char* s, size_t len) {
    uint64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\0' || c == ' ') {
            break;
        }
        result = (result << 3) + static_cast<uint64_t>(c - '0');
    }
    return result;
}
```

逐字符扫描，遇到 null 或空格就停。位移操作 `(result << 3)` 等价于 `result * 
8`，但位运算的语义更明确——我们是在做八进制转换。`c - '0'` 
得到数字的值（假设输入是合法的八进制字符，不做错误检查——ustar 
规范保证字段只包含 '0'-'7' 和终止符）。

这个实现不处理前导空格的特殊情况——如果第一个字符就是空格，函数返回 0，这对 
ustar 的 "全零 size" 字段（`"00000000000"`）和 "空 size" 
字段（全空格）都正确。

### 内部辅助函数

三个匿名命名空间中的辅助函数各自职责清晰：

`is_valid_ustar` 逐字节比较 magic 字段的前 5 个字符。不用 `memcmp` 
是因为它可以更精确地控制比较范围，而且匿名命名空间中的内联函数通常会被编译器完
全内联，性能没有差异。

`data_blocks` 计算文件数据占用的块数。size 为 0 时返回 
0（目录和空文件没有数据块），否则向上取整。这个函数的正确性直接影响 mount 
的跳转逻辑——算错一个块，后面的所有条目都会错位。

`print_bounded` 是一个安全输出函数，逐字符打印最多 max_len 个字符，遇 null 
停止。它解决了一个实际问题：ustar 的 name 字段虽然规范上要求 null 
终止，但当文件名恰好 100 字节时（99 个字符 + 
null）没有问题，但如果某个工具生成了 100 个非 null 字符的 name，直接用 `%s` 
打印就会越界读取。

### mount() 核心逻辑

mount 方法是整个 ramdisk 驱动的核心，按步骤拆解如下：

```cpp
uint32_t Ramdisk::mount() {
    base_ = _binary_initrd_start;
    size_ = static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start);

    if (base_ == nullptr || size_ == 0) {
        cinux::lib::kprintf("[RAMDISK] No initrd archive found.\n");
        return 0;
    }
```

第一步从链接器符号获取归档边界。null 
检查和零大小检查是防御性编程——如果内核构建时没有链接 initrd 
目标文件，符号会指向零地址或不存在（取决于链接器行为），这个检查能防止内核崩溃
。

```cpp
    uint32_t entry_count = 0;
    uint64_t offset = 0;

    while (offset + sizeof(UstarHeader) <= size_) {
        auto* hdr = reinterpret_cast<const UstarHeader*>(base_ + offset);
```

while 条件确保剩余空间至少能容纳一个完整的头部。`sizeof(UstarHeader)` 
编译期等于 512（由 `static_assert` 保证）。指针转换 `reinterpret_cast` 
把原始字节重新解释为头部结构体——这是 packed struct 
的典型用法，结构体字段和磁盘数据精确对应。

```cpp
        if (hdr->name[0] == '\0') {
            break;
        }
        if (!is_valid_ustar(hdr)) {
            cinux::lib::kprintf("[RAMDISK] Invalid ustar magic at offset %u, 
stopping.\n", offset);
            break;
        }
```

两种停止条件：`name[0] == '\0'` 检测到全零头部（end-of-archive 
标记），正常退出循环；magic 
不匹配则说明数据损坏，打印偏移量帮助调试，也退出循环。

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

解析文件大小，根据 typeflag 分类输出。注意 CONTIGUOUS（'7'）和 
REGULAR（'0'）同等对待——两者功能相同。entry_count 只计文件不计目录。

```cpp
        uint32_t blocks = data_blocks(file_size);
        offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * 
USTAR_BLOCK_SIZE;
    }

    cinux::lib::kprintf("[RAMDISK] %u file(s) found in initrd.\n", entry_count);
    return entry_count;
}
```

最后一步是跳转：头部占 512 字节，数据占 blocks * 512 
字节，加起来就是下一个条目的偏移。`static_cast<uint64_t>` 确保乘法不会在 32 
位上溢出（虽然当前 initrd 很小，但写法应该正确支持大文件）。

## 设计决策

### 决策：mount 只做打印不做记录

**问题**: mount 是否应该在遍历时收集所有条目信息？**本项目的做法**: 
只打印不存储，返回文件计数。**备选方案**: 预分配 RamdiskEntry 
数组，遍历时填充。**为什么不选备选方案**: 当前阶段 Cinux 
没有堆分配器，预分配数组需要硬编码最大条目数。打印输出足以验证 initrd 
正确性，条目收集推迟到 VFS 集成时再做更合适。

## 扩展方向

- 实现 `Ramdisk::find(const char* name)` 方法，按文件名查找条目。
- 实现 `Ramdisk::read(const char* name, void* buf, size_t* size)` 
方法，读取文件内容到缓冲区。
- 添加 `Ramdisk::list()` 方法返回所有条目的 RamdiskEntry 数组。
- 支持 initrd 的写入（需要可变大小的归档，复杂度显著增加）。

## 参考资料

- OSDev Wiki: [Initrd](https://wiki.osdev.org/Initrd) — initrd 
使用场景和格式选择
- OSDev Wiki: [Tar](https://wiki.osdev.org/Tar) — tar 格式解析方法
- GNU tar manual: [Format 
Variations](https://www.gnu.org/software/tar/manual/html_node/Formats.html) — 
各种 tar 格式变体
