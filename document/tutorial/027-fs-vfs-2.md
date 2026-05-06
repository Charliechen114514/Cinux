# 027-2 Tutorial: 挂载点表与路径解析 — VFS 的路由器

> 标签：VFS、mount、路径解析、最长前缀匹配、freestanding
> 前置：[027-1 VFS 数据结构](027-fs-vfs-1.md)

## 前言

上一章我们定义了 VFS 的核心数据结构——Inode、FileSystem、File、FDTable。但有一个关键问题一直悬着：当 sys_open("/hello.txt") 被调用时，内核怎么知道应该去找 Ramdisk 还是 ext2？答案是挂载点表（Mount Table）——一个把路径前缀映射到文件系统后端的数据结构，它的角色相当于网络里的路由器：收到一个"请求"（文件路径），根据"目的地址"（路径前缀）决定转发给哪个"下一跳"（文件系统后端）。

你可能会觉得"这不就是一个 map<string, FileSystem*> 吗，有什么难的"。嗯，如果我们有 std::map 确实不难，但别忘了我们在 freestanding 环境下——没有标准库、没有 std::string、没有 std::map。我们连 strlen 都得自己写。所以这一章我们还得先把内核的字符串函数库搞定，然后才能谈挂载点表。这个顺序听起来有点本末倒置，但在 OS 开发中非常常见——基础设施总是先行的，没有桥就过不了河。

## 环境说明

新增文件：`kernel/lib/string.hpp` 和 `string.cpp`（7 个 freestanding 字符串/内存函数）、`kernel/fs/vfs_mount.hpp` 和 `vfs_mount.cpp`（挂载点表实现）。同时修改了 `kernel/arch/x86_64/crt_stub.cpp` 添加 __cxa_guard 支持，修改了 `kernel/CMakeLists.txt` 把新文件加入编译。构建命令不变：`cd build && cmake .. && make`。

## 第一步——先把 strlen 写了：freestanding 的必修课

在写挂载点表之前，我们需要 memset、memcpy、strcmp、strncmp、strlen 这些最基本的函数——挂载点表的路径拷贝和比较全靠它们。在 freestanding 环境下（编译选项 -ffreestanding），标准库不可用，这些函数必须自己实现。这是每一个 OS 内核项目的必经之路——Linux 内核也有自己的 lib/string.c，提供完全相同的一组函数。

九个函数的声明全部放在 `extern "C"` 块里，匹配标准 libc 签名。这样做有两个好处：C++ 代码可以正常调用（链接器会找 C 链接名的符号），而且如果以后有纯 C 的内核代码也能链接到这些函数。memcpy 的 src 和 dest 参数加了 `__restrict__` 关键字——这是 GCC 扩展，告诉编译器两个指针指向的内存不重叠，允许更激进的优化。memmove 没有 __restrict__，因为它必须正确处理重叠。除了 7 个标准函数外，还提供了 strcpy（字符串拷贝）和 utoa（无符号整数转字符串）。

memmove 是七个函数里唯一需要特别处理的——当源和目标有重叠区域时，拷贝方向决定了正确性。我们用最直觉的方式处理：dest < src 时正向拷贝，dest > src 时反向拷贝，dest == src 时什么都不做（直接返回）。Linux 内核的 memmove 也是这么写的，只是它在某些架构上有汇编优化的版本。我们不需要那种级别的优化——逐字节的 C 实现在可读性和正确性上都是最佳选择。

## 第二步——挂载点表：最简单的"路由器"

挂载点表的核心数据结构是一个固定大小的 MountPoint 数组。每个 MountPoint 包含三样东西：path（char[256]，存储挂载路径如 "/" 或 "/mnt"）、fs（FileSystem*，指向具体的文件系统后端实例）、in_use（bool，标记这个槽位是否被占用）。8 个槽位对教学 OS 来说绰绰有余——Linux 桌面系统一般也就十几个挂载点（/, /boot, /home, /tmp, /dev, /proc, /sys, /run, 可能还有 /mnt/usb）。

操作函数有四个，分别对应挂载点管理的四个基本操作。vfs_mount_init 把所有槽位标记为未使用——这一步必须在系统启动时调用，否则后续的 add 和 resolve 行为是未定义的（in_use 可能是内存中的垃圾值 true）。vfs_mount_add 找第一个空位、拷贝路径（用我们自己写的 memcpy）、设置 fs 指针、标记 in_use。vfs_mount_remove 找到匹配路径的条目、置 in_use=false。vfs_resolve 是最核心的函数——最长前缀匹配。

OSDev Wiki 把这种设计叫做"Mount Point List"模型，和另外两种常见模型形成了鲜明对比："Indexed" 模型（DOS/Windows 的盘符，简单但只有 26 个挂载点）和 "Node Graph" 模型（Unix 的 dentry 树，高效但复杂）。Linux 实际上是 Mount Point List 和 Node Graph 的混合体——vfsmount 树管理挂载关系，dentry cache 提供快速路径查找。xv6 则完全不做挂载管理——它只支持一个根文件系统，所有操作都通过 icache（inode cache）直接走磁盘。

## 第三步——vfs_resolve：最长前缀匹配

这是挂载点表最核心也最容易写错的函数。逻辑上分三步：遍历所有 in-use 条目，对每个条目用 strncmp 检查路径是否以挂载路径开头；然后做路径边界检查；记录最长的匹配。

路径边界检查是最容易被忽略的细节，也是 bug 的高发区。如果挂载了 "/mnt"，那路径 "/mnt2/file" 的前四个字符确实是 "/mnt"——但这不是我们想要的匹配。正确的规则是：如果挂载路径不以 '/' 结尾，那路径中紧接前缀的下一个字符必须是 '\0'（路径刚好是挂载点本身，如 "/mnt"）或 '/'（路径在挂载点之下，如 "/mnt/file"）。如果挂载路径以 '/' 结尾（比如 "/"），则不需要额外检查——因为 '/' 本身就是路径分隔符，任何以 '/' 开头的路径都天然在 "/" 之下。

```cpp
char last_mount_char = mpath[mlen - 1];
if (last_mount_char != '/') {
    if (path[mlen] != '\0' && path[mlen] != '/') continue;
}
```

这几行代码看着简单，但写错的话后果很严重：`ls /mnt2` 会列出 `/mnt` 挂载点下的内容——这种 bug 非常隐蔽，只有当两个挂载点路径有前缀关系时才会触发。在只有 "/" 一个挂载点的简单场景下根本测不出来。

最长前缀匹配的选择也有讲究。当 "/" 和 "/mnt" 都有挂载时，路径 "/mnt/file" 应该匹配 "/mnt" 而不是 "/"。这是因为更长的前缀意味着更精确的匹配——就像 IP 路由中的最长前缀匹配一样。我们通过只在 `mlen > best_len` 时更新 best_fs 来实现这一点。

## 第四步——g_global_fd_table 和 __cxa_guard

挂载点表还需要一个全局的 FDTable 访问器。我们用函数内 static 局部变量实现延迟初始化——第一次调用时构造，之后每次返回同一个引用：

```cpp
FDTable& g_global_fd_table() {
    static FDTable s_global_fd_table;
    return s_global_fd_table;
}
```

这行代码看着无害，但它触发了 C++ ABI 中一个不太为人知的机制——__cxa_guard。编译器为每个函数内 static 变量生成一个 64 位的 guard 变量，首次调用时通过 __cxa_guard_acquire 检查是否需要初始化，初始化完成后通过 __cxa_guard_release 标记已初始化。在 freestanding 环境下这两个函数不会被自动提供，必须在 crt_stub.cpp 中手动实现。此外，实际代码中还有一个 current_fd_table() 函数——它会先尝试获取当前任务的 per-process FDTable，不存在时回退到全局 FDTable，为将来的进程隔离做好了准备。

这个坑我踩了半天。编译的时候一切正常，链接的时候突然冒出来两个 undefined reference to __cxa_guard_acquire 和 __cxa_guard_release。搜了一圈才知道这是 Itanium C++ ABI 的一部分——GCC 在编译函数内 static 变量时会自动生成调用。解决方案很简单，在 crt_stub.cpp 中加两个函数：acquire 检查 guard 是否为 0（未初始化），release 设为 1（已初始化）。在单核内核中这完全安全，没有并发问题。多核环境下需要原子操作，但那是以后 028b_sync_safety 的事了。

## 第五步——在 kernel_main 中初始化 VFS

最后一步是在 kernel_main() 中把所有东西串起来。Ramdisk 从局部变量改成了 static——因为它现在包含 64 个 RamdiskEntry（每个约 160 字节），总计约 10KB，放在栈上太大了。mount() 的返回值从 uint32_t 变成了 bool，我们加了一个错误检查。然后依次调用 vfs_mount_init 和 vfs_mount_add。

初始化顺序很重要：先 mount Ramdisk（建立条目表和 inode），再 vfs_mount_init（清空挂载点表），最后 vfs_mount_add（注册 Ramdisk 到 "/" 路径）。如果顺序错了——比如在 mount 之前就 add——那 Ramdisk 的条目表还是空的，后续的 lookup 必定返回 nullptr。

## 收尾

到这里挂载点表已经就位了——vfs_resolve 能把路径路由到正确的文件系统后端，g_global_fd_table 提供全局的文件描述符访问。下一章我们把系统调用全链路串起来，让 sys_open 真正能打开文件、sys_read 真正能读出数据。那时候我们就能在 shell 里 cat 出 initrd 里文件的内容了。

## 参考资料

- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — Mount Point List 模型与三种 VFS 架构的详细对比
- OSDev Wiki: [Hierarchical VFS Theory](https://wiki.osdev.org/Hierarchical_VFS_Theory) — 层次化 VFS 的设计理论和路径查找算法
- Linux Kernel: [vfs_mount](https://docs.kernel.org/filesystems/vfs.html) — Linux 的 vfsmount 树形结构和挂载命名空间
- Itanium C++ ABI: [Guard Variables](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#once-ctor) — __cxa_guard 的完整 ABI 规范和语义定义
- Linux Kernel: [lib/string.c](https://elixir.bootlin.com/linux/latest/source/lib/string.c) — Linux 内核的 freestanding 字符串函数实现
