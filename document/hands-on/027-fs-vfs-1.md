# 027-1 Hands-on: VFS 数据结构设计 — Inode、File 与 FDTable

## 导语

上一个 tag（026）我们实现了 Ramdisk，把 ustar 格式的 initrd 归档解析了出来，能打印出文件名和大小。但说实话，那时候的 Ramdisk 只是一个"解析器"——你没法 open 一个文件、没法 read 它的内容、更没法在 shell 里 cat 出来。整个内核里根本不存在"文件描述符"这个概念，sys_read 只认 fd=0（键盘），sys_write 只认 fd=1（串口）。

现在问题来了：如果我们以后要加 ext2 支持，难道要给 ext2 再写一套独立的 open/read/write？如果同时挂载了 Ramdisk 和 ext2 分区，sys_read 怎么知道该找谁读数据？答案是加一层抽象——Virtual File System，虚拟文件系统。VFS 的核心思想非常简单：定义一套通用的数据结构和接口，所有文件系统后端都实现这套接口，系统调用只跟 VFS 打交道，不关心底层到底是 Ramdisk 还是 ext2 还是网络文件系统。

这一章我们从 VFS 大厦的地基开始——Inode、InodeOps、FileSystem 抽象基类、File 结构体、FDTable。完成本章后，我们会拥有一套可以承载 open/read/write/close 的数据框架，虽然还没有挂载点表和系统调用集成，但接口和表结构已经就位。

知识前置：你需要先完成 026_fs_ramdisk，理解 ustar 归档的解析流程以及 Ramdisk 类的基本结构。

## 概念精讲

### Inode — 文件系统对象的统一表示

在 Unix 世界里，inode 是一切文件系统操作的起点。不管你底层是 ext4、FAT32 还是内存盘，上层看到的都是一个 inode——它代表一个文件或目录，包含编号、大小、类型等元数据。你可以把它理解为一个"文件的身份信息卡"：里面不存文件名（文件名归目录管），但存了文件的物理位置和属性。Linux 内核里的 inode 结构体有几十个字段（权限、时间戳、所属用户等等），我们不需要那么复杂——在教学 OS 里，编号、大小、类型就够用了。

Cinux 的 Inode 有一个特别的字段叫 fs_private——这是一个 void 指针，留给后端文件系统挂自己的私有数据。比如 Ramdisk 可以用它指向内部的 RamdiskEntry（包含文件名和数据指针），而 ext2 可以用它指向磁盘上的块地址列表。这种"公共部分 + 私有指针"的模式在 Linux 里也广泛使用，Linux 的 inode 有一个 i_private 字段做完全一样的事情。

### InodeOps — 函数指针表实现多态操作

InodeOps 在 Cinux 里是一个纯 C 风格的函数指针结构体（struct），不是虚基类。每个字段都是一个函数指针，指向具体的实现函数。不支持的操作就把对应指针设为 nullptr。这种方式在 Linux 内核中也广泛使用——file_operations、inode_operations 都是函数指针表。

InodeOps 函数指针表里定义了多个操作：read（从文件读取数据）、write（向文件写入数据）、readdir（读取目录项）、create（创建文件）、mkdir（创建目录）、unlink（删除文件）、stat（获取文件信息）。每个函数指针都可以为 nullptr——比如普通文件的 ops 不需要设置 readdir（保持 nullptr 即可），而目录的 ops 不需要设置 read（保持 nullptr）。调用方在调用前需要检查函数指针是否为 nullptr。

### File 与 FDTable — 进程视角的打开文件

Inode 是文件系统视角的对象，而 File 是进程视角的对象。为什么要分两层？因为同一个文件可以被同一个进程打开多次——每次 open 都应该有独立的读写位置。如果直接在 Inode 上存 offset，两个 open 同一个文件的 fd 就会互相干扰。Linux 的做法是把这两个概念分开：inode 是全局共享的，file 结构体是每次 open 独立创建的，file 里面存 offset 和指向 inode 的指针。

FDTable 就是进程的文件描述符表，本质上是一个固定大小的指针数组。按照 Unix 传统，fd 0/1/2 分别是 stdin/stdout/stderr——在我们的内核里，fd=0 目前绑定键盘输入，fd=1 绑定 kprintf 输出，fd=2 暂时未用。所以新分配的 fd 从 3 开始。FDTable 的三个核心操作非常直观：alloc 找空位分配、close 释放置空、get 按 fd 编号查找。

## 动手实现

### Step 1: 定义 InodeType 枚举和 InodeOps 函数指针结构体

**目标**: 创建头文件 `kernel/fs/inode.hpp`，定义 Inode 相关的所有类型。

**设计思路**: InodeType 枚举只需要三个值——Unknown(0)、Regular(1) 和 Directory(2)。InodeOps 是一个函数指针结构体（struct），每个字段都是函数指针，如 `int64_t (*read)(const Inode*, uint64_t, void*, uint64_t)`。不支持的操作设为 nullptr。注意 read 的第一个参数是 const Inode*（读不修改 inode），而 write 的第一个参数是 Inode*（写可能修改 size 等元数据）。readdir 的语义比较特殊：返回 1 表示读到一项、0 表示目录已读完、-1 表示错误。

**实现约束**: 在定义 InodeOps 之前，需要先做前置声明 `struct Inode;`，因为函数指针的参数里用到了 Inode*。InodeOps 的函数指针可以为 nullptr，调用方需要在调用前检查对应指针是否为空。

**踩坑预警**: readdir 的 index 参数是 0-based 的索引，不是字节偏移量。调用方（通常是 sys_getdents）需要自己维护这个索引——我们的做法是复用 File 结构体的 offset 字段当索引用，每次 readdir 成功后 offset++。

**验证**: 此步完成后，`kernel/fs/inode.hpp` 应该能独立编译通过（只依赖 stdint.h 和 stddef.h）。检查 sizeof(InodeType) == 1（它是 uint8_t 枚举）。

### Step 2: 定义 Inode 结构体

**目标**: 在同一个 `kernel/fs/inode.hpp` 中，紧接 InodeOps 之后定义 Inode 结构体。

**设计思路**: Inode 有五个核心字段：ino（uint64_t，文件系统特定的编号）、size（uint64_t，文件字节大小）、type（InodeType 枚举）、ops（InodeOps 指针，可为 nullptr）、fs_private（void 指针，后端私有数据）。此外还有 mode/uid/gid/nlink/atime/ctime/mtime/blocks 等为 stat 系统调用预留的字段。所有字段都有默认初始化值。

**实现约束**: Inode 的生命周期由后端文件系统管理——Ramdisk 的 Inode 是嵌入在 RamdiskEntry 里的预分配对象，不需要动态分配。调用方不应该 free/delete 一个 Inode，它归后端所有。

**验证**: `static_assert(sizeof(Inode) > 0)` 应该通过。Inode 的大小约为 96 字节左右（ino 8 + size 8 + type 1 + padding 7 + ops 8 + fs_private 8 + mode 4 + uid 4 + gid 4 + nlink 4 + atime 8 + ctime 8 + mtime 8 + blocks 8）。

### Step 3: 定义 FileSystem 抽象基类

**目标**: 创建头文件 `kernel/fs/vfs_filesystem.hpp`，定义所有文件系统后端的统一接口。

**设计思路**: 这个基类有两个纯虚函数——mount() 负责初始化后端（解析磁盘数据结构、建立索引等），lookup(path) 负责根据相对路径找到对应的 Inode。加上一个虚析构函数 `virtual ~FileSystem() = default` 确保通过基类指针释放时能正确析构。

**实现约束**: mount() 返回 bool（true 成功，false 失败），lookup() 返回 Inode*（找到返回指针，没找到返回 nullptr）。注意 lookup 的参数是相对路径——挂载点前缀由 VFS 上层剥离后传入，所以 Ramdisk 挂在 "/" 下时，lookup 收到的可能是 "hello.txt" 而不是 "/hello.txt"。

**验证**: 编译通过。这个头文件只依赖 inode.hpp 和 stdint.h。

### Step 4: 定义 OpenFlags、File 和 FDTable

**目标**: 创建 `kernel/fs/file.hpp`（声明）和 `kernel/fs/file.cpp`（实现）。

**设计思路**: OpenFlags 枚举值——RDONLY=0, WRONLY=1, RDWR=2——和 Linux 的 O_RDONLY/O_WRONLY/O_RDWR 完全一致，方便用户态直接传值。File 结构体有显式构造函数和三个核心字段：inode 指针、offset（uint64_t，读写偏移量，初始化为 0）、flags（OpenFlags），以及一个 offset_lock_ 自旋锁保护并发访问。FDTable 类有构造函数（初始化所有槽位为 nullptr）、alloc（从 fd=3 开始线性扫描，受 lock_ 保护）、close（边界检查 + delete + 置空，受 lock_ 保护）、get（边界检查 + 返回指针，受 lock_ 保护）、set（强制设置指定 fd 的 File 指针，用于 pipe 和 dup2）。

**实现约束**: FD_TABLE_SIZE 设为 256（与 Linux 默认的 RLIMIT_NOFILE 一致），FD_NONE = -1 作为失败返回值。alloc 的第一个可分配 fd 是 3（跳过 0/1/2）。close 必须做边界检查（fd < 0 或 fd >= 256 都返回 -1）和双重关闭检查（slot 为 nullptr 时返回 -1）。所有公共操作都通过 lock_ 自旋锁保护。

**踩坑预警**: alloc 里用 `new File{...}` 分配对象——这需要内核堆已经初始化。在我们的启动顺序里没问题，但如果你写独立的 host 测试，确保链接了正确的内存分配器。另外，FDTable 目前没有加锁——在单核内核里没问题，但以后多进程/多线程时需要在 alloc 和 close 里加自旋锁保护。

**验证**:
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -5
```
编译应该无错误。如果写了 host 测试：
```bash
make test_fd_table && ./test/unit/test_fd_table
```

### Step 5: 更新 CMakeLists.txt

**目标**: 把新增的源文件加入构建系统。

**设计思路**: 在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 源文件列表中，在 `fs/ramdisk.cpp` 后面加入 `fs/file.cpp` 和 `fs/vfs_mount.cpp`（下章实现，现在可以先加），在 `lib/kprintf.cpp` 后面加入 `lib/string.cpp`。

**验证**:
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | grep -E "(error|warning)" | head -10
```
应该没有 error。

## 构建与运行

此阶段只涉及数据结构定义和 FDTable 实现，还没有系统调用集成。验证方式是编译通过加上 FDTable 的 host 测试。

```bash
cd build && cmake .. && make test_fd_table
ctest -R fd_table --output-on-failure
```

预期输出：24 个测试全部通过，0 failures。

## 调试技巧

**编译报 "undefined reference to operator new"**: 确认内核堆已经初始化。FDTable::alloc 使用 new 分配 File，需要堆就绪。在 host 测试环境下，标准库的 new 是可用的，不会有这个问题。

**InodeOps 函数指针调用崩溃**: 确认 ops 指针和具体函数指针都不为 nullptr 再调用。直接调用空指针会导致页错误或跳转到地址 0。在 syscall handler 里要做三层检查：file != nullptr、file->inode != nullptr、file->inode->ops != nullptr、file->inode->ops->read != nullptr。

**FDTable::alloc 返回 -1**: 检查表是否确实满了——fd 3 到 255 都被分配了？如果刚初始化就返回 -1，说明构造函数可能没正确清空所有槽位。

## 本章小结

| 概念 | 要点 |
|------|------|
| Inode | 文件系统对象的统一表示，ino/size/type/ops/fs_private 五个核心字段 + stat 预留字段 |
| InodeOps | 函数指针结构体 (read/write/readdir/create/mkdir/unlink/stat)，不支持的操作设为 nullptr |
| FileSystem | 抽象基类，mount() 返回 bool + lookup() 返回 Inode* |
| OpenFlags | RDONLY(0)/WRONLY(1)/RDWR(2)，与 Linux O_* 值对应 |
| File | 打开文件描述：绑定 Inode + offset + flags + offset_lock_ |
| FDTable | 256 槽位的文件描述符表，fd 0-2 保留，alloc 从 3 开始，受自旋锁保护 |
