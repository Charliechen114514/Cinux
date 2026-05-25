---
title: 028b-fs-ext2-write-3 · Ext2 写入
---

# 028b-3 Hands-on: 系统调用与 Shell 命令集成

## 导语

前两章我们把 ext2 的底层写入能力全部就位了——分配器能分配和释放块与 inode，create/mkdir/unlink 能创建和删除文件系统对象，write 能往文件里写数据。但这些能力目前只存在于 Ext2 类的内部方法中，Shell 的用户还触摸不到。用户在 shell 里输入 `touch /hello.txt`，这个命令需要经过一整套翻译链条才能到达 Ext2::create——Shell 解析命令，调用 sys_creat 系统调用，内核的 syscall handler 解析路径、找到 VFS 挂载点、定位到父目录 inode、最终调用 InodeOps::create。这条链路上的每一环都必须正确对接，否则 shell 命令就只是一行空文。

这一章要做的事情就是"接通管道"——让 sys_creat、sys_mkdir、sys_unlink、sys_rmdir 四个系统调用走通从用户态到 ext2 后端的完整路径，然后在 Shell 中实现 touch、mkdir、rm、rmdir、echo 五个命令，用它们做端到端验证。中间我们还会遇到一个让人血压拉满的调试案例——syscall 入口的栈对齐 bug，它会以 #GP fault 的面目出现，然后你会花半天时间才定位到问题居然出在 syscall.S 的一行汇编里。

知识前置：028b-1/028b-2（ext2 分配器和文件操作）、027（VFS 框架和系统调用机制）、024（Shell 基础框架）、023（syscall 入口和 dispatch）。

## 概念精讲

### InodeOps 接口扩展

tag 027 的 InodeOps 只定义了 read、write、readdir、stat 四个方法。现在我们要往里面加三个写操作：create、mkdir、unlink。这三个方法的签名是这样的——create 接收父目录 inode、文件名和名字长度，返回新文件的 inode 指针；mkdir 接收同样的参数，返回新目录的 inode 指针；unlink 接收父目录 inode、名字和名字长度，返回 0 表示成功或 -1 表示失败。

基类 InodeOps 提供默认实现——全部返回 -1 或 nullptr。只有实际支持写操作的文件系统后端才需要覆盖这些方法。对于 Cinux 来说，Ext2DirOps 覆盖了 create、mkdir、unlink（因为目录是这些操作的"容器"），Ext2FileOps 不覆盖它们（文件不能包含子文件）。Ramdisk 也添加了对应的实现以保持兼容性。

这里有一个设计上的考量：为什么 create 和 mkdir 是 InodeOps 的方法而不是 FileSystem 的方法？原因是 VFS 在拿到父目录的 inode 之后，直接通过 `parent->ops->create(...)` 调用后端实现，不需要再回到 FileSystem 层。这与 Linux 的 VFS 设计一致——inode_operations 包含 create、lookup、link、unlink 等方法，而 super_operations 负责的是 alloc_inode、destroy_inode 等生命周期管理。职责划分得很清楚。

### 四个新系统调用的统一模式

sys_creat、sys_mkdir、sys_unlink、sys_rmdir 四个系统调用的实现结构几乎是完全一样的，都遵循"路径解析 -> 分割路径 -> 查找父目录 -> 调用 InodeOps 方法"这个模式。我们拆解一下 sys_creat 的流程作为代表：

第一步是路径解析。用户传入的是用户态虚拟地址，我们需要把它拷贝到内核空间（resolve_user_path），同时处理相对路径——如果路径不以 "/" 开头，要拼上当前工作目录。这在 sys_open 里已经实现过了，我们复用同样的逻辑。

第二步是 VFS 挂载点匹配。调用 vfs_resolve 把绝对路径匹配到正确的文件系统后端（比如 ext2 挂载在 "/"），同时剥离挂载点前缀得到相对路径。比如用户传入 "/hello.txt"，vfs_resolve 返回 Ext2 对象和 "hello.txt"。

第三步是路径分割。把 "hello.txt" 分成父目录路径（""，即根目录）和叶名（"hello.txt"）。对于更复杂的路径 "/etc/passwd"，父目录是 "etc"，叶名是 "passwd"。分割算法很简单——从右往左找最后一个 '/'，左边是父路径，右边是叶名。

第四步是查找父目录。调用 fs->lookup(parent_path) 得到父目录的 VFS inode。

第五步是调用后端。`parent->ops->create(parent, leaf_name, name_len)` 触发 Ext2DirOps::create，后者委托给 Ext2::create 完成实际的磁盘操作。

sys_mkdir 和 sys_unlink 的流程与此类似，区别只在最后一步调用的是 ops->mkdir 或 ops->unlink。sys_rmdir 多了一步验证——它会先 lookup 目标路径，检查目标确实是一个目录，并且调用 readdir(index=2) 验证目录是空的（index 0 和 1 分别是 "." 和 ".."，index 2 开始才是实际内容）。如果目录不为空，返回错误。

### syscall GP Fault —— 一个值得铭记的调试案例

在实现 sys_creat 并首次运行 `touch /hello2.txt` 测试时，我遇到了一个让人血压拉满的 bug。QEMU 输出的是一个干干净净的 #GP fault：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  ERROR CODE = 0x0000000000000000
========================================
```

通过 nm 反查 RIP 地址，定位到崩溃发生在 Ext2::create() 内部。再反汇编发现崩溃指令是一条 `movaps %xmm0, (%rax)`——这是 SSE 指令，要求目标地址 16 字节对齐。而当时的 RAX 值末位是 0x78，0x78 % 16 = 8，未对齐，所以 CPU 直接抛 #GP。

但问题来了——Ext2::create() 是普通的 C++ 代码，我没写任何对齐相关的代码，这个 movaps 是编译器自动生成的（用来零初始化栈上的 Ext2Inode 结构体）。编译器之所以敢用 movaps，是因为 SysV AMD64 ABI 保证在 `call` 指令执行前 RSP 是 16 字节对齐的。如果这个不变量被破坏，编译器生成的所有 SSE 指令都可能崩溃。

那谁破坏了对齐？回溯调用链：syscall_entry (asm) -> syscall_dispatch (C) -> ... -> Ext2::create (C)。syscall_entry 中 push 了 12 个寄存器构建 trap frame（96 字节），再 push 第 7 个 C 参数（8 字节），总共 13 次 push = 104 字节。104 % 16 = 8——RSP 偏移了 8 字节，不是 16 的倍数。

修复方法简单到令人发指：在 push 第 7 个参数之前加一条 `subq $8, %rsp`，填充 8 字节把 RSP 对齐到 16 字节边界。修复后，所有相关的栈偏移量都要加 8（因为多了一层 padding），清理时 `addq` 从 $8 改为 $16。

为什么之前的 syscall 没有崩溃？因为此前已注册的系统调用（sys_read、sys_write、sys_open 等）在执行路径上恰好没有触发 SSE 对齐指令——它们的调用栈不够深，或者函数中恰好没有需要 16 字节对齐的栈变量。sys_creat 是第一个深入调用 ext2 复杂逻辑的 syscall，Ext2::create() 中用循环清零 Ext2Inode（编译器优化为 movaps），才首次暴露了这个问题。

这个 bug 的教训是：任何手写的汇编入口（syscall handler、interrupt handler）都必须维护 SysV ABI 的 16 字节栈对齐不变量。push 奇数个 8 字节寄存器会破坏对齐——必须用 subq $8 来补偿。

### Shell 命令实现

五个新 Shell 命令的实现都非常直接——它们就是对应系统调用的薄包装。

touch 调用 sys_creat(path) 创建一个空文件。如果 sys_creat 返回负值就打印错误信息。就这么简单。

mkdir 调用 sys_mkdir(path) 创建目录。

rm 调用 sys_unlink(path) 删除文件。

rmdir 调用 sys_rmdir(path) 删除空目录。

echo 稍微复杂一点——它有两种模式。普通模式是 `echo hello world`，把参数用空格连接后打印到 stdout。重定向模式是 `echo hello > /file.txt`——解析 argv 找到 ">"，把 ">" 前面的内容写入 ">" 后面指定的文件。重定向模式的实现是：先 sys_creat 创建文件，然后 sys_open 打开它（获取 fd），sys_write 把内容写入 fd，最后 sys_close 关闭 fd。

Ramdisk 也需要支持这些写入操作。虽然 Ramdisk 不是我们这次的重点（ext2 才是），但为了 VFS 的完整性，Ramdisk 的 InodeOps 也需要覆盖 create、mkdir、unlink。Ramdisk 的实现更简单——所有数据都在内存里，不需要 bitmap 和 DMA，只需要在内存中的目录项数组里插入或删除条目。

## 动手实现

### Step 1: 扩展 InodeOps 接口

**目标**: 在 inode.hpp 的 InodeOps 基类中添加 create、mkdir、unlink 三个虚函数声明，提供默认实现（返回 -1 或 nullptr）。

**设计思路**: 三个新方法的签名与 Ext2 的对应方法匹配——create 和 mkdir 返回 Inode*，unlink 返回 int64_t。基类默认实现返回 nullptr 或 -1，表示"不支持此操作"。Ext2DirOps 在其头文件和实现文件中覆盖这三个方法，委托给 Ext2 类的 create/mkdir/unlink。

**验证**: 编译通过：
```bash
cd build && cmake .. && make big_kernel_common 2>&1 | tail -10
```

### Step 2: 实现 sys_creat

**目标**: 系统调用号 85，创建新文件。

**设计思路**: resolve_user_path 处理路径 -> vfs_resolve 匹配文件系统 -> split_pathname 分割父路径和叶名 -> fs->lookup 找到父目录 -> parent->ops->create 调用后端。如果 create 返回 nullptr 但文件已存在，回退到 lookup 并截断文件大小为 0（POSIX creat 语义）。

**验证**: 编译通过。

### Step 3: 实现 sys_mkdir

**目标**: 系统调用号 83，创建新目录。

**设计思路**: 与 sys_creat 结构完全一样，最后一步调用 ops->mkdir 而非 ops->create。

**验证**: 编译通过。

### Step 4: 实现 sys_unlink

**目标**: 系统调用号 87，删除文件。

**设计思路**: 同样的路径解析和分割流程，最后调用 ops->unlink。

**验证**: 编译通过。

### Step 5: 实现 sys_rmdir

**目标**: 系统调用号 84，删除空目录。

**设计思路**: 与 sys_unlink 类似，但多了一步验证——先 lookup 目标路径，检查目标存在且是目录类型，然后调用 readdir(index=2) 检查目录是否为空。只有通过所有检查后才调用 ops->unlink。

**验证**: 编译通过。

### Step 6: 注册系统调用

**目标**: 在 syscall_init 的 register_builtin_handlers 中注册四个新系统调用。

**设计思路**: 在 syscall.cpp 中 include 新的头文件（sys_creat.hpp、sys_mkdir.hpp、sys_unlink.hpp、sys_rmdir.hpp），在 register_builtin_handlers 中调用 syscall_register 注册到对应的系统调用号。

**验证**: 编译通过。

### Step 7: 修复 syscall.S 的栈对齐问题

**目标**: 在 syscall_entry 汇编中确保 `call syscall_dispatch` 前 RSP 是 16 字节对齐的。

**设计思路**: 计算 push 的总字节数——12 个通用寄存器 = 96 字节，第 7 个参数 = 8 字节，共 104 字节。104 % 16 = 8，差 8 字节对齐。在 push 第 7 个参数之前加 `subq $8, %rsp` 填充。修复后所有引用栈上参数的偏移量都要加 8（因为多了一层 padding）。

**踩坑预警**: 如果你忘了调整偏移量，syscall_dispatch 收到的参数就会全部错位——第 1 个参数变成了之前第 2 个参数的值，以此类推。这个 bug 的症状是 syscall 的行为完全不可预测，可能报 "invalid syscall nr" 也可能调用到错误的 handler。

**验证**: 编译并在 QEMU 中运行：
```bash
cd build && cmake .. && make big_kernel_common
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr 2>&1 | head -20
# 确认启动时没有 #GP fault
```

### Step 8: 实现 Shell 命令

**目标**: 实现 touch、mkdir、rm、rmdir、echo 五个 Shell 内置命令。

**设计思路**: 每个命令文件（cmd_touch.cpp、cmd_mkdir.cpp 等）实现一个对应的函数，解析 argc/argv，调用对应的系统调用，处理错误返回值。echo 额外支持输出重定向——扫描 argv 中的 ">" 标记，找到后把 ">" 前的参数写入 ">" 后指定的文件。在 shell 的主循环中注册这些新命令。

**验证**: 编译通过。

### Step 9: 端到端 QEMU 测试

**目标**: 在 QEMU 中执行完整的创建、写入、读取、删除操作链。

**验证**:
```bash
# 准备干净的 ext2 镜像
dd if=/dev/zero of=ext2.img bs=1M count=8
mkfs.ext2 -b 1024 ext2.img

# 构建并运行
cd build && cmake .. && make big_kernel_common
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr

# 在 shell 中依次执行以下命令：
# touch /hello.txt
# ls /
# echo Hello from Cinux > /hello.txt
# cat /hello.txt
# mkdir /testdir
# ls /
# ls /testdir
# rmdir /testdir
# ls /
# rm /hello.txt
# ls /
```

预期行为：touch 后 ls 能看到新文件，echo 重定向后 cat 能读到内容，mkdir 后 ls 能看到新目录，rmdir 和 rm 后文件/目录消失。每一步都不应该产生 kernel panic 或异常输出。

### Step 10: 跨重启持久化验证

**目标**: 确认写入的数据在 QEMU 重启后仍然存在（因为 ext2 写入了真实的磁盘镜像）。

**验证**:
```bash
# 上一步中创建文件后正常退出 QEMU（不要 kill）

# 重新启动同一个镜像
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr

# 在 shell 中：
# ls /
# cat /hello.txt   （如果上一步没删除的话）
```

如果之前创建的文件在重启后仍然存在且内容正确，说明我们的写入操作确实持久化到了磁盘。这就是真正的文件系统——数据在断电后依然存活，不像 Ramdisk 那样重启就消失。

## 踩坑预警——常见错误清单

**syscall 栈对齐**: 这是我们遇到的最恶心的 bug。症状是 #GP fault 在某个 C++ 函数内部的 movaps 指令处，但根因在 syscall.S 的入口汇编中。排查方法：GP fault + error_code=0 先怀疑对齐问题，反汇编确认 movaps/movdqa 等对齐指令，回溯栈布局计算 RSP 对齐状态。Intel SDM Vol. 2 中明确说明了 movaps 要求 16 字节内存对齐，违反则触发 #GP(0)。

**sys_rmdir 忘了检查空目录**: 如果 sys_rmdir 不验证目录是否为空就直接调用 unlink，那一个非空目录会被删除，但其包含的文件和子目录的 inode 会变成"孤儿"——它们的 inode 号没有从父目录中移除（因为父目录已经被删了），但也没有被释放。文件系统会产生资源泄漏。

**echo 重定向的 sys_creat + sys_open 序列**: echo 的重定向模式需要先 creat 再 open。如果只 open 不 creat，文件不存在时 open 会失败。如果只 creat 不 open，那就拿不到 fd，无法写入数据。两个调用缺一不可。

**split_pathname 的边界情况**: 对于根目录下直接创建文件（如 "/hello.txt"），split_pathname 应该返回 parent=""（空字符串，代表根目录）和 name="hello.txt"。空字符串传入 fs->lookup 应该返回根 inode。如果 split_pathname 对这种情况处理不当（比如把 "/" 当成了名字的一部分），后续 lookup 就会失败。

**用户态路径指针的拷贝**: resolve_user_path 必须把用户态虚拟地址的字符串拷贝到内核栈上的 buffer 中，不能直接用用户态指针——因为用户态的地址空间和内核不同，直接解引用会访问到错误的内存甚至触发 page fault。我们的实现中 path_util.cpp 负责这个拷贝工作。

## 构建与运行

完整的端到端验证流程：

```bash
# 1. 准备 ext2 镜像
dd if=/dev/zero of=ext2.img bs=1M count=8
mkfs.ext2 -b 1024 ext2.img

# 2. 构建
cd build && cmake .. && make big_kernel_common

# 3. 运行
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr

# 4. Shell 测试命令
# touch /test1.txt
# touch /test2.txt
# echo Cinux ext2 write test > /test1.txt
# cat /test1.txt
# mkdir /docs
# echo readme content > /docs/readme.txt
# cat /docs/readme.txt
# ls /
# ls /docs
# rm /test2.txt
# ls /
# rmdir /docs     <-- 应该失败，因为 docs 不为空
# rm /docs/readme.txt
# rmdir /docs     <-- 现在应该成功
# ls /

# 5. 用宿主机验证（退出 QEMU 后）
e2fsck -f -n ext2.img
dumpe2fs ext2.img 2>/dev/null | grep -E "Free blocks|Free inodes"
```

## 本章小结

| 概念 | 要点 |
|------|------|
| InodeOps 扩展 | 新增 create/mkdir/unlink 虚函数，Ext2DirOps 覆盖实现 |
| sys_creat | 路径解析 + VFS 匹配 + 分割路径 + lookup 父目录 + ops->create |
| sys_mkdir | 同上，最后调用 ops->mkdir |
| sys_unlink | 同上，最后调用 ops->unlink |
| sys_rmdir | 同上，但先验证目标是空目录 |
| syscall 栈对齐 | push 奇数个寄存器破坏 16 字节对齐，用 subq $8 补偿 |
| Shell 命令 | touch/mkdir/rm/rmdir 是系统调用的薄包装，echo 支持重定向 |
| Ramdisk 兼容 | Ramdisk 的 InodeOps 也覆盖 create/mkdir/unlink |
| 持久化验证 | ext2 写入真实磁盘，QEMU 重启后数据仍然存在 |
