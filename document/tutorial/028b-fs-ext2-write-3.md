# 028b-3 Tutorial: 从系统调用到 Shell——完整写入链路

> 标签：系统调用、Shell、sys_creat、sys_mkdir、sys_unlink、GP fault、栈对齐、端到端验证
> 前置：[028b-fs-ext2-write-2](028b-fs-ext2-write-2.md)

## 前言

前两篇文章我们一直在 ext2 驱动内部打转——分配器、元数据回写、目录项操作。这些都是内核里的底层活儿，用户完全感知不到。用户能看到的是 Shell 命令：`touch /hello.txt` 创建文件、`mkdir /subdir` 建目录、`rm /hello.txt` 删文件、`echo hello > /msg.txt` 写内容。从 Shell 命令到 ext2 磁盘操作之间隔着两层——系统调用和 VFS。这一篇我们要把这两层打通，完成从用户态 Shell 到内核态 ext2 的完整写入链路。

说实话，打通这整条链路是我做这个 tag 之前最担心的部分。不是因为代码有多难，而是因为调试太痛苦了——Ring 3 到 Ring 0 的切换、系统调用号分发、VFS 路径解析、ext2 磁盘写入，任何一个环节出了问题都可能导致 kernel panic 或者数据损坏，而且错误信息往往面目全非。果不其然，在这个过程中踩到了一个非常经典的坑——syscall 入口栈对齐问题，它直接导致了一个 GP fault。这个 debug 故事值得专门拿出来讲，因为它可能是所有手写 x86_64 汇编入口的 OS 开发者都会遇到的问题。

## 第一步——InodeOps 接口扩展：从只读到可写

在 tag 027 里我们定义了 `InodeOps` 接口，当时只有 `read`、`write`、`readdir` 三个操作。那时候我们的文件系统是只读的——能读文件内容、能列目录，但不能创建也不能删除。现在我们需要添加三个新操作来支持写入：`create`（在目录中创建文件）、`mkdir`（创建子目录）、`unlink`（删除目录项）。

```cpp
class InodeOps {
public:
    virtual ~InodeOps() = default;
    virtual int64_t read(const Inode* inode, uint64_t offset, void* buf, uint64_t count);
    virtual int64_t write(Inode* inode, uint64_t offset, const void* buf, uint64_t count);
    virtual int64_t readdir(const Inode* inode, uint64_t index, char* name, uint64_t name_max);
    virtual Inode*  create(Inode* dir, const char* name, uint32_t namelen);
    virtual Inode*  mkdir(Inode* dir, const char* name, uint32_t namelen);
    virtual int64_t unlink(Inode* dir, const char* name, uint32_t namelen);
};
```

注意这里有一个设计选择值得讨论：`create`、`mkdir`、`unlink` 为什么定义在 `InodeOps` 里而不是 `FileSystem` 类里？原因在于这些操作的入口是一个"父目录"的 inode——你要在 `/subdir` 下创建 `hello.txt`，调用方手里拿到的是 `/subdir` 的 Inode，而不是什么文件系统级别的句柄。通过 `InodeOps` 的虚函数分发，这个 Inode 的 `ops` 指针指向 `Ext2DirOps`（ext2 目录操作实现），然后由 `Ext2DirOps::create()` 内部调用 `Ext2::create()`。这种分层让 VFS 层的代码完全不需要知道底层是 ext2 还是 ramdisk。

这种通过 inode 的 ops 指针做多态分发的模式，和 Linux 内核的设计高度一致。Linux 的 `inode_operations` 结构体包含 `create`、`lookup`、`link`、`unlink`、`mkdir`、`rmdir` 等函数指针，每个文件系统实例提供自己的实现。C++ 的虚函数在这里起到了和 Linux 的函数指针结构体完全相同的作用——运行时根据对象的实际类型选择正确的实现。区别在于 C++ 的虚函数有隐式的 vtable 指针和 RTTI 开销（每个对象多一个指针，每个类多一个 vtable），但对于我们的场景来说这点开销完全可以忽略。

`Ext2DirOps` 中的实现非常轻量——基本上就是把 VFS Inode 里保存的 inode 号提取出来，然后委托给 `Ext2` 类的对应方法：

```cpp
Inode* Ext2DirOps::create(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return nullptr;
    }
    return ext2_.create(static_cast<uint32_t>(dir->ino), name, namelen);
}
```

`mkdir` 和 `unlink` 的 Ext2DirOps 实现同理——参数校验后委托给 `ext2_.mkdir()` / `ext2_.unlink()`。Ramdisk 也需要提供对应的 `create`/`mkdir`/`unlink` 实现。虽然 ramdisk 的写入能力有限（数据在内存中，关机就丢），但 Shell 的测试路径不应该硬编码 ext2。这是一个重要的架构原则：所有文件系统操作必须通过 VFS 接口进行，任何直接调用 ext2 特定函数的路径都是架构缺陷。将来如果我们加了第三个文件系统（比如 NFS 或者 procfs），它只需要实现 `InodeOps` 接口就能无缝接入——不需要改动 VFS 层和 syscall 层的任何代码。

## 第二步——四个新系统调用

四个系统调用的结构几乎完全一样：路径解析 → VFS 查找 → 调用 InodeOps 方法。拿 `sys_creat` 做代表来拆解完整流程。你会发现这四个 syscall 的代码重复度很高——每个都有相同的 `split_pathname` 函数、相同的路径解析步骤、相同的 VFS lookup 模式。在 Linux 中这些重复代码被抽象到了 VFS 层（`vfs_create`、`vfs_mkdir`、`vfs_unlink` 等辅助函数），但 Cinux 目前的代码量还不足以驱动这种抽象——等重复到忍无可忍的时候再重构也不迟。

第一步是路径解析。用户传入的是一个虚拟地址（Ring 3 的地址空间），需要先通过 `resolve_user_path()` 把它拷贝到内核空间，同时处理 cwd（当前工作目录）前缀。然后通过 `vfs_resolve()` 在挂载表中找到对应的文件系统实例和相对路径。这两个函数是我们在 tag 027/028 中实现的，这里不再赘述。

第二步是路径切分。`split_pathname()` 把完整路径拆成"父目录路径"和"叶子名"。比如 `/subdir/hello.txt` 拆成 `/subdir` 和 `hello.txt`。这个切分是必要的，因为 `create` 操作需要同时操作父目录（插入 entry）和新建 inode——你必须先拿到父目录的 Inode 才能调用 `ops->create()`。切分的逻辑很直观：从右往左找最后一个 `/`，左边是父目录路径，右边是叶子名。如果没有 `/`（比如 `hello.txt`），父目录就是根目录（空字符串）。

```cpp
// split_pathname 的核心逻辑
int32_t last_sep = -1;
for (uint32_t i = 0; i < len; ++i) {
    if (path[i] == '/') {
        last_sep = static_cast<int32_t>(i);
    }
}
if (last_sep < 0) {
    // 没有 /：父目录是根目录
    parent_out[0] = '\0';
    *name_out = path;
    *namelen_out = len;
} else {
    // 切分：左边是父路径，右边是叶子名
    memcpy(parent_out, path, last_sep);
    parent_out[last_sep] = '\0';
    *name_out = path + last_sep + 1;
    *namelen_out = len - last_sep - 1;
}
```

第三步是 VFS 查找。调用 `fs->lookup(parent_buf)` 找到父目录的 Inode，然后通过 `parent->ops->create()` 调用底层的创建逻辑。

`sys_mkdir` 的结构和 `sys_creat` 完全一致，只是调用的是 `parent->ops->mkdir()` 而不是 `create()`。`sys_unlink` 同样，调用 `parent->ops->unlink()`。这三个 syscall 的差异仅在最后一步——调用不同的 InodeOps 方法。

`sys_rmdir` 比 `sys_unlink` 多了一步验证。它先 lookup 目标路径确认存在且是目录，然后尝试 readdir 索引 2（跳过 "." 和 ".."）——如果返回了有效 entry 说明目录非空，拒绝删除：

```cpp
// sys_rmdir 的空目录检查
cinux::fs::Inode* target = fs->lookup(rel_path);
if (target->type != cinux::fs::InodeType::Directory) {
    return -1;  // 不是目录
}
// 尝试读取索引 2 的 entry（跳过 "." 和 ".."）
char check_name[16];
int64_t rc = target->ops->readdir(target, 2, check_name, sizeof(check_name));
if (rc > 0) {
    kprintf("[SYS_RMDIR] '%s' is not empty\n", resolved);
    return -1;  // 目录非空
}
// 验证通过，调用 unlink 删除
parent->ops->unlink(parent, leaf_name, name_len);
```

这是一种非常简洁的空目录检测方法。readdir 索引 0 和 1 分别返回 "." 和 ".."（在 `Ext2DirOps::readdir` 中硬编码处理），索引 2 是第一个真正的用户 entry。如果索引 2 返回 0（没有更多 entry），说明目录里只有 "." 和 ".."，是空的。Linux 的 `sys_rmdir` 做的事情本质上一样——检查目录是否只有 "." 和 ".."——但它的实现路径更复杂（`vfs_rmdir` → `may_delete` → 检查 `empty_dir`），还处理了挂载点、权限、immutable flag 等边界情况。

## 第三步——那场 GP Fault：栈对齐杀了我一个下午

四个系统调用写完之后，我信心满满地在 QEMU 里敲了 `touch /hello.txt`。然后收获了这样一个惊喜：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  ERROR CODE = 0x0000000000000000
========================================
```

General Protection Fault。error_code 是 0，说明不是段权限问题，是"其他"原因——在 x86_64 上，error_code=0 的 GP fault 最常见的成因就是对齐违规。用 `nm` 反查 RIP 落在 `Ext2::create()` 内部，离函数入口偏移 0x77 字节。反汇编一看：

```asm
ffffffff81005d7d:  lea    0x80(%rsp),%rdx    ; new_disk 栈变量地址
ffffffff81005d8e:  mov    %rdx,%rax
ffffffff81005d97:  movaps %xmm0,(%rax)       ; ← GP fault 在这里
```

`movaps` 是 SSE 的对齐移动指令（Move Aligned Packed Single-precision），要求操作数是 16 字节对齐的。如果地址不是 16 的倍数，CPU 直接抛 GP fault——不是降级处理，不是默默修正，是直接炸。Intel SDM Vol. 2 对 `movaps` 的说明非常明确："When the source or destination operand is a memory operand, the operand must be aligned on a 16-byte boundary or a #GP(0) exception will be generated." 注意它的未对齐兄弟指令 `movups`（Move Unaligned Packed Single-precision）没有这个限制，但 GCC 在能确定对齐的情况下会优先选择 `movaps`，因为它在某些微架构上更快。

RAX = RSP + 0x80，按理说 0x80 是 16 的倍数，所以如果 RSP 本身是 16 的倍数，RAX 也应该是。回溯 RSP 的值，发现 RSP 的最后一位十六进制数字是 8——不是 0。RSP % 16 == 8，差了 8 字节。

根因在 `syscall_entry` 汇编里。我们的 syscall 入口 push 了 12 个寄存器（构建 trap frame，96 字节），然后再 push 第 7 个 C 参数（8 字节），总共 104 字节。进入 `syscall_entry` 时 RSP 是 16 字节对齐的（SysV ABI 保证进入 `syscall` 指令时栈是对齐的），push 104 字节后 RSP 偏移了 104。104 % 16 = 8——`call syscall_dispatch` 之前 RSP 不是 16 的倍数，违反了 SysV AMD64 ABI 的栈对齐要求。

你可能会问：之前的 syscall（read、write、open）为什么没崩？因为它们的执行路径恰好没有触发 SSE 对齐指令。`sys_read` 和 `sys_write` 的逻辑很简单——检查 fd、调用底层驱动——编译器用普通的 `mov` 指令就够处理那些小的局部变量。`sys_creat` 是第一个深入调用 ext2 复杂逻辑的 syscall，`Ext2::create()` 中用循环清零 `Ext2Inode` 结构体（128 字节），GCC 14 足够聪明地把它优化为几条 `movaps`（每次写 16 字节，8 次写完 128 字节），这才首次暴露了对齐 bug。可以说这个 bug 一直在那里潜伏着，只是等待一个足够深的调用栈来触发它。

修复很简单——在 push 第 7 个参数之前加 `subq $8, %rsp` 填充 8 字节：

```asm
movq 72(%rsp), %rax                # 从 trap frame 加载第 6 个用户参数
subq $8, %rsp                      # ← 新增：16 字节对齐
pushq %rax                         # push 7th arg（作为 C 函数的第 7 个参数，通过栈传递）
```

但加了这 8 字节后，下面所有从 trap frame 读取参数的偏移量都要从 +8 调整为 +16（因为栈上多了一层 padding）。比如原来 `movq 32(%rsp), %rdi`（读取 syscall number）现在变成 `movq 40(%rsp), %rdi`。清理时也要从 `addq $8` 改为 `addq $16`（清除参数加 padding）。这个改动看起来很小，但改偏移量的过程极其容易出错——差一个字节就又是 GP fault。我改完后数了三遍偏移量才敢编译。

这个 bug 的教训非常深刻，值得展开讲几点。

第一，x86_64 的 SysV ABI 假定 `call` 指令前 RSP 是 16 的倍数。这不是"建议"或者"最佳实践"，而是硬性约定——编译器生成的代码（特别是涉及 SSE/AVX 指令的代码）会无条件依赖这个前提。第二，对齐 bug 不在崩溃函数中，而在调用链最顶层的汇编入口中——这种"bug 在 A 处，症状在 B 处"的模式在 OS 开发中非常常见。第三，对齐 bug 有隐蔽性——简单的 syscall（浅调用栈）不会触发，只有深入调用且碰上 SSE 指令时才暴露。所以千万不要觉得"之前的 syscall 能跑就说明汇编入口没问题"——那只是还没碰到触发条件而已。第四，排查方法要形成条件反射：GP fault + error_code=0 → 先看是否为对齐问题 → 反汇编确认 `movaps`/`movdqa` 等对齐指令 → 回溯栈布局计算 RSP 对齐状态。

Linux 的 syscall 入口（`arch/x86/entry/entry_64.S`）使用宏 `PUSH_AND_CLEAR_REGS` 来保存所有寄存器，这个宏 push 了奇数个寄存器后也做了对齐处理。xv6 的 trap 处理同样需要考虑这个问题。可以说这是所有手写 x86_64 汇编入口的 OS 开发者都会遇到的经典陷阱。

## 第四步——Shell 命令实现：薄薄的用户态封装

Shell 命令的实现非常直接——每个命令就是一个解析参数、调用系统调用、打印结果的薄封装。`cmd_touch` 调用 `sys_creat(argv[1])`，`cmd_mkdir` 调用 `sys_mkdir(argv[1])`，`cmd_rm` 调用 `sys_unlink(argv[1])`，`cmd_rmdir` 调用 `sys_rmdir(argv[1])`。这些命令的错误处理都很简陋——失败时只打印 "cannot create 'xxx'"，不区分具体错误原因（文件已存在？权限不够？磁盘满？）。POSIX 标准要求返回具体的 errno（EEXIST、EACCES、ENOSPC 等），但 Cinux 的 syscall 目前只返回 -1 表示失败，不传递错误码。这是一个已知的不足，将来需要改进。

```cpp
void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        write_str("touch: missing file operand\n");
        return;
    }
    const char* path = argv[1];
    int64_t result = sys_creat(path);
    if (result < 0) {
        write_str("touch: cannot create '");
        write_str(path);
        write_str("'\n");
    }
}
```

比较有意思的是 `cmd_echo` 的重定向功能。当参数中包含 `>` 时，echo 把前面的文本写入到 `>` 后面的文件路径：

```cpp
// 检测重定向标记
int redirect_idx = -1;
for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], ">") == 0) {
        redirect_idx = i;
        break;
    }
}
if (redirect_idx > 0 && redirect_idx + 1 < argc) {
    // 重定向模式：echo hello world > /msg.txt
    sys_creat(path);                           // 创建目标文件
    int64_t fd = sys_open(path, 1);            // O_WRONLY 打开
    for (int i = 1; i < redirect_idx; ++i) {
        sys_write(fd, argv[i], strlen(argv[i])); // 写入每个参数
    }
    sys_close(fd);
}
```

这个实现虽然简陋（不支持 `>>` 追加、不支持管道 `|`、不支持 `2>` 重定向 stderr、不支持变量展开 `$HOME`），但已经足以演示从 Shell 到 ext2 磁盘写入的完整链路了。用户输入 `echo hello > /msg.txt`，Shell 调用 `sys_creat` 创建文件、`sys_open` 打开文件、`sys_write` 写入内容、`sys_close` 关闭文件。每一次系统调用都穿过 Ring 3 到 Ring 0 的 syscall 入口，经过 VFS 路径解析，最终到达 ext2 的 `create()` 和 `write()` 方法，把数据真正写到硬盘上。

回想一下我们从头到尾走过的路径：Shell 解析命令 → 用户态调用 `sys_creat` → `syscall` 指令触发 Ring 3→0 切换 → `syscall_entry` 汇编保存寄存器、构建 trap frame → `syscall_dispatch` 根据系统调用号分发到 `sys_creat` → 路径解析 + VFS lookup → `Ext2DirOps::create()` → `Ext2::create()` → `alloc_inode()` + `write_disk_inode()` + `add_dir_entry()` → AHCI DMA 写入硬盘。十几步的调用链，每一步都可能有 bug，但一旦全部跑通，那种从用户输入到磁盘写入的端到端体验是非常有成就感的。

## 第五步——Ramdisk 写入支持：为测试保留的后门

ext2 的写入链路需要真实的硬盘（AHCI + DMA），在单元测试中不太方便。所以我们给 ramdisk 也加了 write 支持——`RamdiskFileOps::write()` 直接把数据拷贝到内存中的归档数据缓冲区。ramdisk 的 create/mkdir/unlink 则操作内存中的 ustar 归档结构。

Ramdisk 的写入能力和 ext2 相比有一个本质区别：ramdisk 的数据在关机后丢失（内存嘛），而 ext2 的数据持久化在硬盘上。但对于快速验证 VFS + syscall 链路的正确性来说，ramdisk 写入是一个非常好用的测试工具——不需要挂载硬盘镜像、不需要 mkfs，直接在内存里操作。Cinux 的单元测试（`test/unit/test_shell_write.cpp`）就是通过 ramdisk 来验证 Shell 命令的正确性的。

这里有一个架构上的启发：多层抽象的真正价值不在于让你能"用多种方式做同一件事"，而在于让你能"用一种方式测试多种实现"。VFS 层让我们可以用 ramdisk 测试 syscall + Shell 链路，用 ext2 测试磁盘写入链路——两条路径共享同一套 VFS 代码，确保了语义一致性。

## 第六步——端到端验证：在 QEMU 里跑通完整写入链路

所有代码写完之后，验证流程是这样的。首先用 `mkfs.ext2` 创建一个小的 ext2 镜像，挂到 QEMU 的 AHCI 端口。启动内核，确认 ext2 mount 成功（串口日志应该打印 superblock 信息和 BGDT 加载信息）。然后进入 Shell 执行以下测试序列：

1. `touch /test.txt` — 创建一个空文件，确认无报错
2. `ls /` — 应该能看到 test.txt 出现在目录列表中
3. `mkdir /subdir` — 创建一个子目录
4. `ls /` — 应该能看到 test.txt 和 subdir
5. `echo hello > /msg.txt` — 重定向写入
6. `cat /msg.txt` — 应该显示 "hello"
7. `rm /test.txt` — 删除文件
8. `ls /` — test.txt 应该消失
9. `rmdir /subdir` — 删除空目录
10. `ls /` — subdir 应该消失

验证的关键点不在"能不能创建文件"——如果我们前面的代码正确的话这是理所当然的。真正的验证点在元数据一致性：创建和删除操作后 superblock 的空闲计数器对不对、bitmap 的位有没有正确更新、目录项的 `rec_len` 链接是否完整。这些在 QEMU 里可以通过串口日志来确认——我们在 `alloc_block`、`alloc_inode`、`create`、`mkdir`、`unlink` 等函数中都加了 kprintf 日志。如果每次操作的日志都符合预期（bitmap 更新、计数器变化、inode 写入），那基本可以确认文件系统状态是正确的。

有一个特别值得关注的测试场景是 `mkdir /subdir` 之后 `rmdir /subdir`——这涉及 links_count 的完整生命周期：mkdir 时父目录的 `i_links_count +1`（因为子目录的 ".." 指向它），rmdir 时父目录的 `i_links_count -1`。如果这两步不对称，几次 mkdir/rmdir 循环之后父目录的 links_count 就会飘掉。root 目录的初始 links_count 通常是 2（自身 + "."），每次 mkdir 加 1，每次 rmdir 减 1。如果连续 mkdir/rmdir 10 次后 root 的 links_count 不等于 2，那说明某个地方有 bug。

## 收尾

到这里，Cinux 的 ext2 写入支持就完整了。从最底层的块分配器、inode 分配器，到中间层的目录项操作和文件写入，再到上层的系统调用和 Shell 命令，整条链路已经可以跑通。用户可以在 Shell 里创建文件、建目录、写内容、删文件——虽然功能还很原始（没有权限检查、没有并发控制、没有日志、没有 mmap），但这是一个"能用"的文件系统了。说实话第一次在 QEMU 里看到 `touch /hello.txt` 成功执行、`ls /` 显示出新创建的文件时，我的感觉就像第一次看到内核成功启动一样激动。

回头看这三个 tag 的代码量——ext2.cpp 从 028 tag 的约 900 行增长到约 1800 行，新增了 alloc_block/free_block、alloc_inode/free_inode、create/mkdir/unlink、add_dir_entry/remove_dir_entry、get_or_alloc_block、write_block/write_superblock/write_bgdt/write_disk_inode 等一整套写入基础设施。加上四个新系统调用和五个 Shell 命令，整个 028b tag 的代码增量在 1000 行左右。这个量级对于一个教学内核来说是合理的——每行代码都有明确的目的，没有过度设计也没有偷懒省略。

下一步（028c tag）我们会给文件系统加上 cwd（当前工作目录）支持和 stat 系统调用。有了 cwd 之后，Shell 就不用每次都输入完整路径了——`cd /subdir` 然后 `touch hello.txt`，这才是一个正常的使用体验。stat 则让我们能查看文件的元数据（大小、权限、时间戳），为后续的 `ls -l` 命令做准备。这两项改进会让 Cinux 的文件系统体验从"能用"变成"好用"。

## 参考资料

- [OSDev Wiki - ext2](https://wiki.osdev.org/Ext2) — ext2 数据结构参考，Superblock/BGDT/Inode 字段定义
- Intel SDM Vol. 2, `movaps` 指令 — 要求 16 字节内存对齐，否则触发 #GP(0)。Intel SDM Vol. 2, Chapter 4 "Instruction Set Reference", `MOVAPS` 条目
- Intel SDM Vol. 2, Chapter 3 — `syscall`/`sysret` 指令行为，RCX 保存 RIP、R11 保存 RFLAGS 的机制
- [System V Application Binary Interface AMD64](https://gitlab.com/x86-psABIs/x86-64-ABI) — x86_64 SysV ABI 栈对齐要求：`call` 前 RSP 必须是 16 的倍数（第 3.2.2 节 "The Stack Frame"）
- [Linux kernel entry_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S) — Linux 的 syscall 入口汇编，同样需要处理栈对齐（通过 `PUSH_AND_CLEAR_REGS` 宏保证）
- [xv6 fs.c](https://github.com/mit-pdos/xv6-public/blob/master/fs.c) — xv6 的文件系统实现，包含 balloc/bfree（块分配器）和 dirlookup/dirent（目录操作）
