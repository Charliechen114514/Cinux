---
title: 027-fs-vfs-4 · VFS 虚拟文件系统
---

# 027-4 Tutorial: Ramdisk 适配、用户态集成与测试策略 — 让 shell 里 cat 出文件内容

> 标签：Ramdisk、shell、cat、ls、测试、FDTable、VFS 集成
> 前置：[027-3 系统调用全链路](027-fs-vfs-3.md)

## 前言

前三章我们把 VFS 的数据结构、挂载点表、系统调用全链路都实现了。内核侧的 open/read/write/close/getdents 已经能正确地通过 VFS 找到 Ramdisk 里的文件并读取数据。但说实话，一个 OS 项目如果不能在终端里跑起来给人看，那和"纸上谈兵"没什么区别。你跟别人说"我实现了一个 VFS 层"，人家问你"那你能 cat 一个文件吗？"——如果答案是"不能"，那就有点尴尬了。

这一章我们要做最后两块拼图：用户态的 shell 命令（cat 和 ls）和一套完整的测试矩阵。完成后，你应该能在 Cinux 的 shell 里输入 `cat /hello.txt` 看到文件内容被打印出来，输入 `ls /` 看到所有文件名列出来。至此，milestone 027 的完整目标——open/read/write/close syscall 框架可用——就达成了。

测试这一块值得单独拿出来说。我们在 027 tag 中新增了两套 host 测试（test_fd_table 24 个用例、test_vfs_mount 19 个用例）和一套 kernel 集成测试（test_vfs_syscall 28 个用例），加上 ramdisk 测试中新增的 17 个 VFS 相关用例。总共 88 个新测试用例——这个覆盖率对于一个 milestone 来说是比较扎实的，而且测试代码（约 1600 行）占整个 tag 代码量（3408 行）的近一半，体现了"测试驱动开发"的理念。

## 环境说明

新增文件：`user/programs/shell/cmd_cat.cpp` 和 `cmd_ls.cpp`（两个 shell 命令实现）、`user/libc/syscall.cpp` 中新增三个 wrapper（sys_open/sys_close/sys_getdents）、`test/unit/test_fd_table.cpp`（24 个 host 测试）、`test/unit/test_vfs_mount.cpp`（19 个 host 测试）、`kernel/test/test_vfs_syscall.cpp`（28 个 kernel 测试）。`test/CMakeLists.txt` 新增两个 host 测试目标。

## 第一步——Ramdisk 适配的细节：预分配 Inode 和 RamdiskDirContext

虽然上一章讲了 Ramdisk 继承 FileSystem 的大方向，但有几个实现细节值得展开。第一个是预分配 Inode 的生命周期管理——RamdiskEntry 内嵌的 Inode 归 Ramdisk 对象所有，调用方不应该 delete 它。这意味着 sys_close 只需要释放 FDTable 中的 File 对象（它里面有 Inode 指针但不拥有 Inode），不需要递归释放 Inode 本身。Linux 的 inode 生命周期管理更复杂（通过 i_count 引用计数），但我们的简化版本在单进程内核中完全够用。

第二个细节是 RamdiskDirContext 的设计动机。readdir 的函数指针指向 ramdisk.cpp 匿名命名空间里的实现函数，不能直接访问 Ramdisk 类的私有成员 entries_ 和 entry_count_。通过 fs_private 指向 RamdiskDirContext（包含 entries 指针和 count），readdir 就能遍历条目表了。这种"通过上下文结构传递信息"的模式在 Linux 内核中也广泛使用——很多 file_operations 的实现通过 file->private_data 传递私有信息。

## 第二步——shell 命令：cat 和 ls

cat 命令的实现逻辑是 Unix cat 的最小子集——打开文件、循环读取、输出到 stdout、关闭文件。从命令参数中提取路径（跳过 "cat " 前缀），sys_open 打开文件，分配 256 字节的栈缓冲区，循环 sys_read。每次读取后通过 sys_write(fd=1) 输出——fd=1 的 handler 是 kprintf，所以 cat 的输出自动走串口。这个设计和 xv6 的 cat.c 几乎一模一样（cat(int fd) { while((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n); }），只是我们加了错误提示。

ls 命令和 cat 类似但用 getdents 替代 read。打开目录路径（默认 "/")后循环 sys_getdents——每次调用返回一个目录项名称和名称长度，ls 把名称加上换行符输出到 stdout。getdents 返回 0 时目录遍历完毕。这里有个小细节：Ramdisk 的 readdir 保证先返回 "." 和 ".."，然后按条目表顺序返回文件名。这种可预测的顺序让测试更容易写——我们知道第三个 getdents 调用一定返回第一个真实文件的名字。

## 第三步——测试矩阵：三层金字塔

我们的测试策略是三层金字塔结构——底层是快速单元测试，中层是集成测试，顶层是手动验证。每一层覆盖不同的错误维度，合在一起形成完整的质量保障。

最底层是 host 单元测试——test_fd_table（24 个用例）和 test_vfs_mount（19 个用例）。它们在开发机上直接编译运行，不需要 QEMU，秒级完成，适合快速迭代。test_fd_table 覆盖了 FDTable 的全部操作，特别值得注意的是压力测试——"填满 253 个槽位 → 关闭偶数编号 → 重新分配"这个测试验证了线性扫描最低可用 fd 的策略在部分释放后仍然正确。这个场景模拟了真实使用中频繁 open/close 的行为，是 double-close 和 use-after-free bug 的高发区。

test_vfs_mount 使用 MockFileSystem（mount 返回 true、lookup 返回 nullptr）测试挂载点表逻辑，重点验证最长前缀匹配。当同时挂载 "/" 和 "/mnt" 时，resolve("/mnt/file") 必须返回 "/mnt" 挂载点的 FileSystem 而非 "/" 的。这个测试是整个挂载点表最重要的 correctness property——如果最长前缀匹配错了，文件操作会路由到错误的文件系统。

中间层是 kernel 集成测试——test_vfs_syscall（28 个用例）。它在 QEMU 内核中运行，使用真实的 Ramdisk（包含 hello.txt、readme.txt、etc/passwd 三个文件），验证从 syscall handler 到 VFS 到 Ramdisk 的完整路径。每个测试用 setup_vfs() 创建干净的环境——new Ramdisk、mount、vfs_mount_add("/")，测试完后 teardown_vfs 清理。这种隔离模式确保测试之间互不影响。

最有价值的测试是"完整生命周期"组中的两个用例。第一个验证 Ramdisk 只读语义：open("/hello.txt") 成功 → read 返回 "Hello from Cinux!\n" → write 返回 -1（只读） → close 成功 → 再次 read 返回 -1（fd 已关闭）。这条链路覆盖了 open/read/write/close 四个 syscall 的核心行为。第二个验证多文件操作：同时 open 两个文件、交错读取、全部关闭后都变无效。这个测试验证了 FDTable 在多个 fd 同时使用时不会串位或混淆。

## 第四步——变更统计和架构回顾

027_fs_vfs 总共修改了 43 个文件，新增 3408 行，删除 155 行。按功能分类：核心实现代码（Inode/FileSystem/FDTable/挂载点表/InodeOps/syscall）约 1200 行，测试代码约 1600 行（host 672 行 + kernel 692 行 + ramdisk 测试扩展 340 行），用户态和构建系统约 600 行。测试代码占比接近 50%，这在操作系统开发中是健康的比例。

从架构上看，这个 tag 完成了 Cinux 文件系统的基础设施建设。Inode/InodeOps（函数指针表）定义了文件系统对象的统一表示和操作接口，FileSystem 抽象基类定义了后端的接口契约，挂载点表提供了路径路由能力，FDTable 管理文件描述符，五个系统调用完成了用户态到内核态的桥梁。这些组件为下一个 tag（028_fs_ext2）铺平了道路——ext2 只需要继承 FileSystem、实现 mount/lookup、提供自己的 InodeOps 实例、注册到挂载点表，就能无缝接入现有的 syscall 框架。不需要改 sys_open 的一行代码。

## 收尾

到这里 milestone 027 就大功告成了。我们从一个只能打印文件名的 Ramdisk（026），走到了一个完整的 VFS 框架（027）——Inode 抽象了文件对象，FileSystem 抽象了文件系统后端，挂载点表路由了路径到后端，五个系统调用串联了用户态和内核态，shell 里 cat 和 ls 可以用了，88 个测试用例保驾护航。下一个 tag 我们会把这个 VFS 框架接到真正的 ext2 文件系统上，让 Cinux 能读写 QEMU 磁盘上的文件。

完结撒花。

## 参考资料

- xv6: [sysfile.c](https://github.com/mit-pdos/xv6-public/blob/master/sysfile.c) 和 [cat.c](https://github.com/mit-pdos/xv6-public/blob/master/cat.c) — xv6 的系统调用和 cat 实现，Cinux 的设计与之直接对比
- MIT 6.S081: [File System Lab](https://pdos.csail.mit.edu/6.S081/2023/labs/fs.html) — xv6 文件系统实验，教学 OS 测试策略的参考
- Linux Kernel: [Overview of VFS](https://docs.kernel.org/filesystems/vfs.html) — Linux VFS 的测试和验证方法论
- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — VFS 设计中 open/close/read/write 的完整语义描述
- "A Virtual Filesystem Layer in XV6" (PDF): [链接](https://maups.github.io/papers/tcc_004.pdf) — 在 xv6 上实现 VFS 层的学术研究
