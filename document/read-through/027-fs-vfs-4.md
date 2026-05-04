# 027-4 Read-through: 用户态集成与测试代码 — shell 命令和测试矩阵

## 概览

本文讲解 027 tag 的最后一层——用户态 shell 命令（cat/ls）和测试代码（host 测试 + kernel 测试）。这一层是把所有内核侧的机制转化为用户可见功能的关键：cat 命令让用户能读出文件内容，ls 命令让用户能列出目录。测试代码则确保从 syscall handler 到 VFS 到 Ramdisk 的每一层都按预期工作。

关键设计决策：cat/ls 命令通过 fd=1 (stdout) 输出而不是直接 kprintf；host 测试和 kernel 测试分层验证不同级别的正确性；测试矩阵覆盖正常路径和所有错误路径。

## 代码精讲

### 用户态 libc wrapper

在 `user/libc/syscall.cpp` 中新增了三个 wrapper 函数，遵循已有的 syscall 调用约定——系统调用号放入 rax，参数按序放入 rdi/rsi/rdx/r10/r8/r9，执行 syscall 指令，返回 rax 的值。

sys_open 的 wrapper 接收路径指针和 flags，sys_close 接收 fd，sys_getdents 接收 fd、缓冲区指针和缓冲区大小。三个 wrapper 的系统调用号分别对应 SYS_open=2、SYS_close=3、SYS_getdents=78，和 kernel 侧的 SyscallNr 枚举完全一致。

CMakeLists.txt 中也需要把新的 shell 命令源文件加入用户态程序的编译列表。cmd_cat.cpp 和 cmd_ls.cpp 被添加到 shell 可执行文件的源文件中，shell.hpp 中声明了对应的处理函数，main.cpp 的命令分发表中注册了 "cat" 和 "ls" 两个新命令。

### shell cat 命令

cat 命令的实现逻辑是 Unix cat 的最小子集——打开文件、循环读取、输出到 stdout、关闭文件：

流程上，先从命令行字符串中提取文件路径（跳过 "cat " 前缀和可能的空格），然后 sys_open 打开文件。如果 open 返回 -1，直接打印 "Failed to open" 提示。打开成功后，分配 256 字节的栈缓冲区，循环调用 sys_read——每次读取最多 256 字节，读到的内容通过 sys_write(fd=1) 输出到终端。sys_read 返回 0 表示 EOF，退出循环；返回 -1 表示读错误，打印提示后退出。最后 sys_close 关闭 fd。

这里有一个细节值得注意：sys_write 的 fd=1 路径是 kprintf——所以 cat 的输出会走串口，在 QEMU 的 -serial stdio 中显示。这就是为什么我们不需要额外的"打印到屏幕"的机制。

cat 命令还需要处理一个边缘情况：文件大小可能不是 256 的整数倍，最后一次 read 返回的字节数会小于缓冲区大小。正确的做法是只 write 实际读到的字节数，而不是固定 256 字节。我们的实现通过 sys_read 的返回值来决定 sys_write 的长度，天然处理了这个问题。

### shell ls 命令

ls 命令比 cat 稍微复杂——它需要遍历目录项而不是读取文件数据。流程是：打开目录路径（默认 "/"），循环 sys_getdents 读取目录项名称，每个名称后加换行符通过 sys_write(fd=1) 输出，getdents 返回 0 时退出，最后 close。

getdents 的特殊之处在于它每次只返回一个目录项的名称——不是像 Linux getdents 那样一次填充多个 linux_dirent 结构。这个简化设计让 shell 实现更简单，但性能上稍差（每个目录项需要一次 syscall）。对 Ramdisk 里的小量文件来说完全够用。

### Host 测试: test_fd_table (24 个用例)

test_fd_table 是纯粹的 FDTable 单元测试，直接链接 kernel/fs/file.cpp，在开发机上运行。测试使用项目自带的 TEST_FRAMEWORK，每个测试用 `TEST("description") { ... }` 宏定义，断言用 ASSERT_EQ/ASSERT_TRUE/ASSERT_NULL/ASSERT_NOT_NULL。

测试按功能分为 11 组。构造测试验证所有 256 个槽位初始化为 nullptr——遍历 get(0) 到 get(255) 全部返回 nullptr。顺序分配测试验证 alloc 返回 3、4、5（从 fd=3 开始递增）。表满测试填满所有 253 个可分配槽位（3-255），验证第 254 次 alloc 返回 FD_NONE。重用测试验证 close(fd=4) 后下一次 alloc 返回 4（最低可用 fd）。

边界条件测试覆盖了 close 的各种错误情况——负数 fd 返回 -1、超出表大小的 fd 返回 -1、已关闭的 fd 再次 close 返回 -1、从未分配的 fd close 返回 -1。get 的测试类似——越界返回 nullptr、关闭后返回 nullptr。

File 字段存储测试验证 alloc 时传入的 inode 指针和 OpenFlags 被正确保存在 File 对象中，offset 初始化为 0。这里用一个 dummy Inode（ino=42）来验证指针的正确绑定。

压力测试做了完整的生命周期验证：填满所有 253 个槽位 → 关闭所有 → 验证全部 nullptr → 重新填满。还有一个"关闭偶数槽位后重新分配"的测试——先填满 3-255，然后 close 所有偶数 fd（4、6、8...），再重新 alloc 验证这些偶数槽位被依次复用。这测试了线性扫描最低可用 fd 的核心逻辑在高压力下是否正确。

### Host 测试: test_vfs_mount (19 个用例)

test_vfs_mount 使用一个简单的 MockFileSystem 类（mount 返回 true，lookup 返回 nullptr）来测试挂载点表逻辑，不需要真实的文件系统后端。测试覆盖：

初始化后 resolve 任何路径返回 nullptr。添加单个 "/" 挂载点后 resolve "/" 成功。添加多个挂载点后都能正确 resolve。填满 8 个槽位后再次 add 返回 false。nullptr 路径或 nullptr 文件系统返回 false。空字符串路径返回 false。

remove 测试验证删除后 resolve 返回 nullptr，删除不存在的路径返回 false。

resolve 的最长前缀匹配是重点测试——同时挂载 "/" 和 "/mnt" 时，"/mnt/file" 应该匹配到 "/mnt" 而非 "/"。无匹配时返回 nullptr。g_global_fd_table 多次调用返回同一个对象的引用（验证 static 局部变量的延迟初始化）。

### Kernel 测试: test_vfs_syscall (28 个用例)

test_vfs_syscall 在 QEMU 内核中运行，验证从 syscall handler 到 VFS 到 Ramdisk 的完整路径。每个测试用 setup_vfs() 创建干净的环境——vfs_mount_init + new Ramdisk + mount + vfs_mount_add("/")，测试完后 teardown_vfs 清理。

7 个测试组按功能划分。挂载注册组（4 个）测试 init/add/resolve 的基本流程和参数校验。sys_open 组（6 个）测试有效路径返回 fd、不存在文件返回 -1、null 路径返回 -1、空路径返回 -1、非规范地址返回 -1、无挂载点返回 -1。

sys_read 组（5 个）测试读取返回正确数据（验证 "Hello from Cinux!\n" 的内容）、offset 更新后第二次 read 从新位置开始、读完后再 read 返回 0（EOF）、无效 fd 返回 -1、close 后 read 返回 -1。

sys_write 组（3 个）测试 Ramdisk 写返回 -1、无效 fd 返回 -1、close 后 write 返回 -1。sys_close 组（2 个）测试正常关闭返回 0 和无效 fd 返回 -1。

生命周期组（2 个）是最有价值的集成测试：第一个测试完整的 open→read→write(fail)→close→read(fail) 流程，验证 Ramdisk 的只读语义；第二个测试同时打开两个文件、交错读取、全部关闭后都变无效的场景。

sys_getdents 组（6 个）验证目录遍历的顺序（先是 "."、".."，然后是文件名）、目录遍历完毕返回 0、无效 fd 返回 -1、close 后返回 -1、null 缓冲区返回 -1、非规范地址返回 -1。

值得注意的是 setup_vfs/teardown_vfs 辅助函数的设计——每个测试创建一个全新的 Ramdisk 实例和干净的挂载点表，确保测试之间完全隔离。setup_vfs 做三件事：vfs_mount_init 清空挂载点表、new Ramdisk 并 mount、vfs_mount_add 注册到 "/"。teardown_vfs 做清理：vfs_mount_remove("/")、delete Ramdisk。这种模式虽然每个测试都有重复的 setup 开销（Ramdisk 要重新解析 ustar 归档），但保证了测试的独立性和可重复性。

### 测试中发现的变更

027 tag 还对之前的测试代码做了若干适配性修改。test_ramdisk.cpp 中 RamdiskEntry 的 name 字段从 const char* 改成了 char 数组，所以 host 测试中设置 entry.name 的方式从指针赋值变成了 memcpy 拷贝。test_syscall.cpp 和 test_shell.cpp 中 sys_write 的地址校验测试从 `buf_virt >= USER_ADDR_MAX` 改成了 `buf_virt == 0`（null pointer check），因为我们的地址校验逻辑从简单上限变成了 canonical address 规则——null 是最常见的非法地址，需要单独拒绝。test_ramdisk.cpp 中还新增了大量 VFS 相关的测试（lookup 7 个、InodeOps 5 个、VFS 集成 5 个），总计增加了约 340 行测试代码。

## 设计决策

### 决策：简化 getdents vs POSIX getdents

**问题**: sys_getdents 的接口设计。

**本项目的做法**: 每次调用返回一个目录项名称（字符串），用 file->offset 做索引。

**备选方案**: 按照 POSIX getdents 返回 linux_dirent 结构体数组（包含 d_ino、d_off、d_reclen、d_name）。

**为什么不选备选方案**: linux_dirent 结构需要计算 d_reclen（含对齐填充）和 d_off（下次 seek 的位置），实现复杂度高，而且我们的 shell 只需要文件名。简化后的接口每次只返回一个名称字符串，足够用且容易实现。

### 决策：分层测试策略

**问题**: 测试如何组织？

**本项目的做法**: Host 单元测试（FDTable 和 vfs_mount）+ Kernel 集成测试（全链路 syscall）。

**备选方案**: 全部在 QEMU 内核测试中运行。

**为什么不选备选方案**: Host 测试编译快、运行快、不需要 QEMU，适合快速迭代。只有涉及 syscall 指令和真实 Ramdisk 数据的测试才需要 kernel 测试。分层策略把测试速度最大化，同时保证集成覆盖。

## 扩展方向

- 添加 stat 命令——显示文件大小和类型（难度：⭐）
- 实现 pipe——连接 cat 和 ls 的输出到另一个命令的输入（难度：⭐⭐⭐）
- 支持 cat 多个文件——`cat /a.txt /b.txt` 连续输出（难度：⭐）
- 为 test_vfs_syscall 添加并发测试——多个 fd 同时读写（难度：⭐⭐）

## 参考资料

- xv6: [file.c](https://github.com/mit-pdos/xv6-public/blob/master/file.c) — filealloc/fileclose/sysfile.c 中的 sys_open/sys_close
- Linux: [getdents(2)](https://man7.org/linux/man-pages/man2/getdents.2.html) — POSIX getdents 系统调用的完整语义
- MIT 6.S081: [File System Lab](https://pdos.csail.mit.edu/6.S081/2023/labs/fs.html) — xv6 文件系统实验
