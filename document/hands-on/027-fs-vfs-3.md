---
title: 027-fs-vfs-3 · VFS 虚拟文件系统
---

# 027-3 Hands-on: Ramdisk VFS 适配与系统调用 — 让 open/read/write 跑起来

## 导语

前两章我们搭好了数据结构（Inode/File/FDTable）和挂载点表（vfs_mount/vfs_resolve），但这一切都还只是"框架"——没有任何代码真正地把 open 系统调用和 VFS 串联起来。这一章我们就来做这件事：让 Ramdisk 继承 FileSystem 抽象基类、实现 mount() 和 lookup()、提供三组 InodeOps 函数指针表（读归档数据、写返回错误、遍历目录项），然后新增 sys_open/sys_close/sys_getdents 三个系统调用，同时改造 sys_read/sys_write 让它们在 fd != 0/1 时走 VFS 路径。

完成本章后，内核的 open/read/write/close 全链路就打通了——从用户态的 libc wrapper 到 syscall dispatch，再到 VFS resolve、FileSystem lookup、InodeOps 回调，最终到 Ramdisk 的数据拷贝。在 QEMU 里你应该能看到 shell 的 cat 命令打印出 initrd 里文件的内容。

知识前置：027-1（VFS 数据结构）、027-2（挂载点表）、026（Ramdisk 解析）。

## 概念精讲

### Ramdisk 适配 — 从"解析器"到"文件系统"

在 026 里，Ramdisk::mount() 只是遍历 ustar 归档打印文件名，返回条目数量。现在我们需要把它升级成一个真正的文件系统后端：mount() 要构建一个内部条目表，每个条目包含文件名、数据指针、大小，以及一个预分配的 Inode。lookup() 根据文件名在条目表里做线性搜索。这种设计简单粗暴——线性搜索 O(n) 在条目数少的时候完全够用，我们的 RAMDISK_MAX_ENTRIES 只有 64。

关键变化是 RamdiskEntry 结构：之前 name 字段是一个 const char*（指向 ustar header 里的 name），现在改成了 char 数组（拷贝出来，不依赖 header 的生命周期），并且内嵌了一个 Inode 对象。这样每个文件条目都自带自己的 inode，不需要额外分配内存。同时新增了 RamdiskDirContext 结构来辅助 readdir——它保存条目表指针和条目数量，让 readdir 函数不需要直接访问 Ramdisk 的私有成员。Ramdisk 提供两个静态 InodeOps 实例——ramdisk_file_ops（普通文件操作）和 ramdisk_dir_ops（目录操作），每个实例的函数指针指向对应的实现函数，不支持的操作保持 nullptr。

### 地址校验 — 从 USER_ADDR_MAX 到 Canonical Address

之前的 sys_read/sys_write 用一个简单的 `buf_virt >= 0x800000000000` 来拒绝内核地址。这个检查有问题：x86_64 的规范地址空间不是在某个固定值一刀切的——48 位线性地址空间的高半部分（内核空间）地址是 0xFFFF800000000000 到 0xFFFFFFFFFFFFFFFF，而不是从 0x800000000000 开始。

x86_64 的 canonical address 规则是：bit 47 决定了 bits 48-63 应该全是 0 还是全是 1。如果 bit 47 是 0，那 bits 48-63 必须全为 0；如果 bit 47 是 1，那 bits 48-63 必须全为 1。我们用这个规则替换了简单的上限检查，同时把 nullptr（地址 0）单独拒绝。

### 系统调用分发更新

新增的三个系统调用号：SYS_open=2、SYS_close=3、SYS_getdents=78。open 的流程是 vfs_resolve 找后端、fs->lookup 找 inode、FDTable::alloc 分配 fd。close 就是 FDTable::close。getdents 比较特殊——它通过 fd 找到 File，再用 file->inode->ops->readdir 读取目录项，用 file->offset 当目录项索引（每次成功后 offset++）。这个设计虽然不太 POSIX（真正的 getdents 返回的是 linux_dirent 结构体），但对我们的简单 shell 来说够用了。

## 动手实现

### Step 1: 让 Ramdisk 继承 FileSystem

**目标**: 修改 `kernel/fs/ramdisk.hpp`，让 Ramdisk 类继承 FileSystem 抽象基类。

**设计思路**: 在类声明中加上 `: public FileSystem`，把 mount() 的返回类型从 uint32_t 改为 bool，新增 `Inode* lookup(const char* path) override` 方法。新增私有成员：entries_ 数组（RamdiskEntry[64]，预分配条目表）、entry_count_（条目数量）、root_inode_（根目录 inode，用于 readdir）、root_ctx_（RamdiskDirContext，用于 readdir）。Ramdisk 提供两个静态 InodeOps 实例（ramdisk_file_ops 和 ramdisk_dir_ops），每个实例的函数指针指向 ramdisk.cpp 匿名命名空间里的实现函数。

**实现约束**: RAMDISK_MAX_ENTRIES = 64，RAMDISK_NAME_MAX = 100。RamdiskEntry 的 name 字段从 const char* 改为 char[100]（拷贝文件名，不依赖 ustar header 的生命周期），内嵌 Inode inode 字段。

**踩坑预警**: RamdiskEntry 的内存布局变了——之前只有三个指针字段（name/size/data），现在加了 Inode（约 40 字节）和 name 数组（100 字节），每个条目变成了约 160 字节。entries_ 是 64 个条目的数组，总共约 10KB。对于内核栈来说太大了，所以 Ramdisk 对象必须分配在堆上或作为 static 变量（我们在 main.cpp 中用了 static）。

**验证**: 编译通过。

### Step 2: 实现 Ramdisk 的 InodeOps

**目标**: 在 `kernel/fs/ramdisk.cpp` 中实现 ramdisk_file_ops 和 ramdisk_dir_ops 两个静态 InodeOps 实例。

**设计思路**: ramdisk_file_ops 设置 read 函数指针：从 inode->fs_private 拿到 RamdiskEntry 指针，从 entry->data + offset 拷贝 min(count, entry->size - offset) 字节到用户缓冲区。write 函数指针设为 nullptr（只读文件系统）。ramdisk_dir_ops 设置 readdir 函数指针：从 inode->fs_private 拿到 RamdiskDirContext，index=0 返回 "."，index=1 返回 ".."，index>=2 返回 entries[index-2].name。两个 ops 实例都放在匿名命名空间里，避免符号泄漏。

**实现约束**: ramdisk_read 需要检查 offset >= size 的情况——此时返回 0（EOF）。ramdisk_readdir 的 index 参数直接用 uint64_t，不做任何转换。两个 InodeOps 实例放在匿名命名空间里，作为静态全局变量直接定义（`static InodeOps ramdisk_file_ops = { .read = ramdisk_read, .write = nullptr, ... }`），不需要 new 分配。

**踩坑预警**: ramdisk_read 的 buf 参数是 void*（内核地址），在 syscall handler 中已经由 sys_read 从用户态虚拟地址转换过来了。但注意，我们现在直接把用户态地址传给 InodeOps——这在 kernel test 中工作正常（因为测试用的地址就是内核地址），但在真正的用户态 syscall 中，这个地址是用户态虚拟地址，需要确保页表映射正确。目前的实现依赖 QEMU 的 identity mapping，暂无问题。

**验证**: 编译通过。运行 kernel 测试中的 test_ramdisk 部分——应该有新增的 lookup 和 InodeOps 测试通过。

### Step 3: 改造 Ramdisk::mount() 和实现 lookup()

**目标**: 更新 mount() 以构建条目表，新增 lookup() 方法。

**设计思路**: mount() 的主循环不变（遍历 ustar 条目），但不再是只打印文件名——而是把每个文件的信息填入 entries_ 数组，同时设置对应的 Inode（ino=序号、size=文件大小、type=Regular、ops=&ramdisk_file_ops、fs_private=&entry）。循环结束后设置 root_inode_ 和 root_ctx_。lookup() 处理三种情况：空路径或 "/" 返回根目录 inode；否则跳过前导 '/' 后线性搜索条目表。

**实现约束**: mount() 最后返回 entry_count_ > 0。lookup() 对 nullptr 路径返回 nullptr。root_inode_ 的 type 设为 Directory，ops 指向 ramdisk_dir_ops，fs_private 设为 &root_ctx_。

**验证**: 编译并运行 kernel 测试。test_ramdisk 部分应该有 7 个 lookup 测试和 5 个 InodeOps 测试通过。

### Step 4: 实现 sys_open

**目标**: 创建 `kernel/syscall/sys_open.cpp` 和 `sys_open.hpp`。

**设计思路**: 四步流程——第一步调用 resolve_user_path() 做路径解析（内部包含 canonical address 校验和 cwd 感知的路径规范化）；第二步调用 vfs_resolve 找到对应的 FileSystem 后端；第三步调用 fs->lookup(rel_path) 找到 Inode；第四步把 flags 映射到 OpenFlags 后调用 current_fd_table().alloc() 分配 fd。

**实现约束**: resolve_user_path() 封装在 kernel/syscall/path_util.hpp 中，内部做 canonical address 检查（提取 bit47，如果 bit47=0 则 upper 必须为 0，如果 bit47=1 则 upper 必须为 0xFFFF）并将路径解析为绝对路径。flags 映射：0→RDONLY, 1→WRONLY, 2→RDWR，其他默认 RDONLY。使用 current_fd_table() 而非 g_global_fd_table()，支持 per-process FDTable。

**踩坑预警**: path 参数是用户态传上来的虚拟地址，sys_open 直接 reinterpret_cast 为 const char*。这个地址在用户态页表中必须有效——如果用户传了一个无效地址（比如已经 free 掉的内存），内核会页错误。在当前的简化实现中，我们先不处理这种情况（以后加 page fault handler 时再完善）。

**验证**: 编译通过。

### Step 5: 实现 sys_close

**目标**: 创建 `kernel/syscall/sys_close.cpp` 和 `sys_close.hpp`。

**设计思路**: 极其简单——调用 current_fd_table().close(fd)，把返回值转成 int64_t 返回。所有边界检查和双重关闭检查都在 FDTable::close 里做。

**验证**: 编译通过。

### Step 6: 实现 sys_getdents

**目标**: 创建 `kernel/syscall/sys_getdents.cpp` 和 `sys_getdents.hpp`。

**设计思路**: 先做 canonical address 校验，然后通过 current_fd_table() 拿到 File，检查 ops->readdir 是否存在。在 offset_lock_ 保护下调用 readdir(inode, file->offset, buf, count)，成功（返回 1）时 offset++ 并返回名称长度，目录结束（返回 0）时返回 0，错误（返回 -1）时返回 -1。

**实现约束**: buf_virt 参数是用户态虚拟地址，直接 reinterpret_cast 为 char* 后传给 readdir。file->offset 在这里被复用为目录项索引——这是一个巧妙但有点 hacky 的设计，offset 本来是文件读写偏移量，对于目录 fd 来说它变成了目录项的 0-based 索引。

**验证**: 编译通过。

### Step 7: 更新 sys_read 和 sys_write — VFS 路径

**目标**: 修改现有的 `kernel/syscall/sys_read.cpp` 和 `sys_write.cpp`。

**设计思路**: sys_read 和 sys_write 都采用"VFS 优先"策略——先查 FDTable 看这个 fd 是否有对应的 File，如果有就走 VFS 路径；如果没有，再检查 fd=0（键盘）或 fd=1（kprintf）的 legacy 路径。这种设计比简单的 fd==0/fd!=0 分支更灵活——它允许 fd=0 被重新分配为 pipe 或其他 VFS 后端。

**实现约束**: sys_read 的 VFS 路径：先通过 current_fd_table().get(fd) 获取 File，检查三层非空（file/inode/ops），然后在 offset_lock_ 保护下调用 ops->read(file->inode, file->offset, buf, count)，返回值 > 0 时 file->offset += result。sys_write 同理。

**踩坑预警**: 别忘了在 sys_read 里保留 fd=0 的键盘路径——否则 shell 的命令输入就断了！在 sys_write 里保留 fd=1 的 kprintf 路径——否则所有 printf 输出都消失了。

**验证**: 编译并运行全量 kernel 测试。

### Step 8: 注册新系统调用

**目标**: 修改 `kernel/arch/x86_64/syscall.cpp` 和 `kernel/syscall/syscall_nums.hpp`。

**设计思路**: 在 syscall_nums.hpp 中新增 SYS_open=2、SYS_close=3、SYS_getdents=78。在 register_builtin_handlers() 中注册这三个新 handler。

**验证**:
```bash
cd build && cmake .. && make big_kernel_test
qemu-system-x86_64 -serial stdio -kernel big_kernel_test.bin 2>&1 | grep -E "(PASS|FAIL|VFS)"
```

## 构建与运行

```bash
cd build && cmake .. && make test_host && make big_kernel_test
```

Host 测试（test_fd_table + test_vfs_mount）+ kernel 测试（test_vfs_syscall，23 个用例覆盖 open/close/read/write/getdents 全链路）应该全部通过。

## 调试技巧

**sys_open 返回 -1 但路径确实存在**: 检查 vfs_resolve 的输出——如果 rel_path 不对（比如多了个前导 '/' 或少了字符），lookup 自然找不到。加个 kprintf 打印 resolve 后的 rel_path 看看。

**sys_read 返回 -1**: 最可能的原因是 ops 或 ops->read 为 nullptr——检查 inode 的 ops 指针是否正确设置了。另一个可能是 buf 地址不合法（canonical address 检查不过）。

**getdents 只返回 "." 和 ".." 但没有文件**: 检查 root_ctx_ 的 entries 和 count 是否正确设置——如果 mount() 里没有正确设置 root_ctx_，readdir 拿到的 count 就是 0，自然读不到文件。

## 本章小结

| 概念 | 要点 |
|------|------|
| Ramdisk 继承 FileSystem | mount() 返回 bool，新增 lookup() 线性搜索，静态 ops 实例 |
| InodeOps | ramdisk_file_ops（read 拷贝数据/write 为 nullptr）、ramdisk_dir_ops（readdir 返回目录项） |
| sys_open | resolve_user_path → vfs_resolve → lookup → current_fd_table().alloc |
| sys_close | 直接调用 current_fd_table().close(fd) |
| sys_getdents | fd → File → Inode → ops->readdir，offset 当索引用 |
| sys_read/write 更新 | fd=0/1 保留旧逻辑，其他 fd 走 VFS |
| canonical address | bit47 决定 bits 48-63 应全为 0 或全为 1 |
