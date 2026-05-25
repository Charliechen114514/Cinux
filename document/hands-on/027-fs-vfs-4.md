---
title: 027-fs-vfs-4 · VFS 虚拟文件系统
---

# 027-4 Hands-on: 用户态集成与测试验证 — shell 里 cat 出文件内容

## 导语

上一章我们把内核侧的 open/read/write/close/getdents 全链路打通了，系统调用能正确地通过 VFS 找到 Ramdisk 里的文件并读取数据。但这条链路还需要最后两块拼图才能在 shell 里真正使用：用户态的 libc wrapper（让用户程序能调用 open/close/getdents）和 shell 命令实现（cat 和 ls）。同时，我们需要一套完整的测试矩阵来验证从 syscall handler 到 VFS 到 Ramdisk 的每一层都工作正常。

完成本章后，你应该能在 Cinux 的 shell 里输入 `cat /hello.txt` 看到文件内容被打印出来，输入 `ls /` 看到所有文件名列出来。至此，milestone 027 的完整目标——open/read/write/close syscall 框架可用——就达成了。

知识前置：027-3（Ramdisk 适配与系统调用）、024（Shell 基础框架）。

## 概念精讲

### 用户态 libc wrapper — 桥接用户程序和内核

用户态程序不能直接调用内核函数——它们通过 syscall 指令触发系统调用。libc wrapper 的作用就是封装这个指令调用过程：把函数参数装进正确的寄存器（rdi/rsi/rdx/r10/r8/r9），执行 syscall，返回结果。对于 open/close/getdents 三个新系统调用，我们需要在 `user/libc/syscall.cpp` 和 `syscall.h` 中添加对应的 wrapper 函数。

### shell cat 命令 — open→read→write→close

cat 命令的实现逻辑非常直接：用 sys_open 打开用户指定的路径，循环 sys_read 读取文件内容到缓冲区，每次读取后用 sys_write(fd=1) 把内容打印到终端，读完后 sys_close 关闭文件描述符。这里有一个需要注意的点：sys_read 返回 0 表示 EOF（文件读完），返回 -1 表示错误——两种情况都要处理，但只有错误需要打印提示。

### shell ls 命令 — open→getdents loop→close

ls 命令比 cat 稍微复杂一点：用 sys_open 打开目录路径（我们的 Ramdisk 根目录 "/" 是一个 type=Directory 的 inode），然后循环调用 sys_getdents 读取目录项名称。每次 getdents 成功后，把名称打印出来（sys_write fd=1 加换行），直到 getdents 返回 0 表示目录遍历完毕。最后 close fd。

### 测试矩阵 — 分层验证

我们的测试策略是分三层：第一层是 host 单元测试（test_fd_table 和 test_vfs_mount），在开发机上直接运行，验证 FDTable 和挂载点表的逻辑正确性；第二层是 kernel 集成测试（test_ramdisk 中的 VFS 部分和 test_vfs_syscall），在 QEMU 内核里运行，验证从 syscall 到 VFS 到 Ramdisk 的完整路径；第三层是手动验证——在 shell 里实际执行 cat 和 ls 命令看输出。

## 动手实现

### Step 1: 添加用户态 libc wrapper

**目标**: 在 `user/libc/syscall.cpp` 和 `syscall.h` 中添加 sys_open、sys_close、sys_getdents 的 wrapper。

**设计思路**: 每个 wrapper 遵循相同的模式——把系统调用号放入 rax，参数放入 rdi/rsi/rdx/r10/r8/r9，执行 syscall 指令，返回 rax 的值。sys_open 的参数是路径指针和 flags；sys_close 只有一个 fd 参数；sys_getdents 参数是 fd、缓冲区指针和缓冲区大小。注意 sys_open 在内核侧会通过 resolve_user_path() 自动处理 cwd 解析。

**实现约束**: 系统调用号必须和 kernel 侧的 SyscallNr 枚举一致（SYS_open=2, SYS_close=3, SYS_getdents=78）。返回值类型是 int64_t，-1 表示错误。

**验证**: 用户态程序编译通过。

### Step 2: 实现 shell cat 命令

**目标**: 创建 `user/programs/shell/cmd_cat.cpp`。

**设计思路**: 从命令行参数中提取文件路径，调用 sys_open 打开文件，然后循环 sys_read（每次读 256 字节）到缓冲区，每次读完后 sys_write(fd=1, buf, bytes_read) 打印内容。sys_read 返回 0 时退出循环，返回 -1 时打印错误信息并退出。最后 sys_close(fd)。

**实现约束**: cat 命令需要在 shell 的命令分发表中注册——识别 "cat" 前缀后提取后面的路径参数。路径参数可能带前导空格，需要跳过。缓冲区大小建议 256 字节——对 Ramdisk 里的小文件足够了。

**踩坑预警**: sys_open 的路径参数必须是绝对路径（以 '/' 开头），因为我们的 vfs_resolve 依赖前导 '/' 来匹配挂载点。如果用户输入 "hello.txt" 而不是 "/hello.txt"，vfs_resolve 找不到任何挂载点，sys_open 会返回 -1。

**验证**: 在 QEMU shell 中输入 `cat /hello.txt`，预期看到 "Hello from Cinux!" 被打印出来。

### Step 3: 实现 shell ls 命令

**目标**: 创建 `user/programs/shell/cmd_ls.cpp`。

**设计思路**: 从参数中提取目录路径（默认 "/"），sys_open 打开目录，循环 sys_getdents 读取目录项名称，每读到一个就 sys_write 打印出来（加换行符）。sys_getdents 返回 0 时退出循环。最后 sys_close。

**实现约束**: sys_getdents 每次调用返回一个目录项的名称长度，名称被写入用户提供的缓冲区。目录项的顺序由 Ramdisk 的 readdir 实现决定——先是 "." 和 ".."，然后是 Ramdisk 中的所有文件。

**验证**: 在 QEMU shell 中输入 `ls /`，预期看到 "."、".."、"hello.txt"、"readme.txt"、"etc/passwd" 每行一个被打印出来。

### Step 4: 注册 shell 命令

**目标**: 修改 `user/programs/shell/shell.hpp` 和 `main.cpp`，注册 cat 和 ls 命令。

**设计思路**: 在 shell 的命令分发表中添加 "cat" 和 "ls" 条目，分别指向对应的处理函数。同时更新 help 命令的输出列表。

**验证**: shell 中输入 `help` 应该能看到 cat 和 ls 的说明。

### Step 5: 编写 host 测试 — test_fd_table

**目标**: 创建 `test/unit/test_fd_table.cpp`，验证 FDTable 的所有操作。

**设计思路**: 测试覆盖 11 个分类——构造（所有槽位 nullptr）、顺序分配（3/4/5）、表满返回 FD_NONE、释放后重用最低 fd、正常关闭、无效 fd 关闭、get 正常/越界/关闭后、File 字段存储正确、压力测试（填满→关闭一半→重分配）。

**实现约束**: 共 24 个测试用例。使用项目自带的 TEST_FRAMEWORK（和之前的 host 测试一致）。链接 kernel/fs/file.cpp，不需要任何硬件模拟。

**验证**:
```bash
cd build && cmake .. && make test_fd_table
./test/unit/test_fd_table
```
预期：24 个测试全部通过。

### Step 6: 编写 host 测试 — test_vfs_mount

**目标**: 创建 `test/unit/test_vfs_mount.cpp`，验证挂载点表的所有操作。

**设计思路**: 需要一个 MockFileSystem（简单的 FileSystem 子类，mount 返回 true，lookup 返回 nullptr）。测试覆盖——init 后表为空、添加单个/多个挂载点、表满返回 false、nullptr 参数拒绝、正常删除、删除不存在的返回 false、精确匹配和最长前缀匹配、无匹配返回 nullptr、g_global_fd_table 返回同一引用。

**实现约束**: 共 19 个测试用例。链接 kernel/fs/vfs_mount.cpp 和 kernel/fs/file.cpp。

**验证**:
```bash
cd build && cmake .. && make test_vfs_mount
./test/unit/test_vfs_mount
```
预期：19 个测试全部通过。

### Step 7: 编写 kernel 集成测试 — test_vfs_syscall

**目标**: 创建 `kernel/test/test_vfs_syscall.cpp`，在 QEMU 内核中验证全链路。

**设计思路**: 每个测试用 setup_vfs() 初始化环境（vfs_mount_init + new Ramdisk + mount + vfs_mount_add），测试完后 teardown_vfs() 清理。7 个测试组——挂载注册（4 个）、sys_open（6 个）、sys_read（5 个）、sys_write（3 个）、sys_close（2 个）、完整生命周期（2 个）、sys_getdents（6 个），共 28 个用例。

**实现约束**: 所有 syscall 函数通过 reinterpret_cast 把内核地址当参数传入（因为是内核态测试，地址本身就是有效的）。注意 path 参数需要是 const char* 再转成 uint64_t。

**验证**:
```bash
cd build && cmake .. && make big_kernel_test
qemu-system-x86_64 -serial stdio -kernel big_kernel_test.bin 2>&1 | grep "VFS Syscall"
```
预期看到测试部分打印，全部 PASS。

## 构建与运行

完整的验证流程：

```bash
cd build && cmake .. && make test_host
ctest --output-on-failure
```

然后启动内核（非 test 版本）验证 shell 命令：

```bash
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin
# 在 shell 中输入：
# ls /
# cat /hello.txt
# cat /readme.txt
```

## 调试技巧

**cat 命令返回 -1 但文件确实存在**: 用 kprintf 在 sys_open 里打印 resolve 的结果——如果 vfs_resolve 返回 nullptr，说明路径没有匹配到任何挂载点。最常见的原因是用户输入了相对路径（没有前导 '/'）。

**ls 输出只有 "." 和 ".."**: 检查 Ramdisk 的 mount() 是否成功——如果 initrd 为空或 mount 失败，root_ctx_.count 为 0，readdir 在 "." 和 ".." 之后就结束了。

**host 测试链接失败**: test_fd_table 和 test_vfs_mount 需要链接正确的 kernel 源文件。检查 test/CMakeLists.txt 中的 target_sources 和 include_directories 设置。

**kernel 测试中 new 返回 nullptr**: 确认内核堆已经初始化。test_vfs_syscall 中 setup_vfs() 使用了 `new Ramdisk()`，需要堆就绪。在 main_test.cpp 中确认 run_vfs_syscall_tests() 在堆初始化之后被调用。

## 本章小结

| 概念 | 要点 |
|------|------|
| libc wrapper | 封装 syscall 指令，匹配 kernel 侧系统调用号 |
| cat 命令 | open → read loop → write(fd=1) → close |
| ls 命令 | open → getdents loop → write(fd=1) → close |
| test_fd_table | 24 个 host 测试，覆盖 FDTable 全操作 |
| test_vfs_mount | 19 个 host 测试，覆盖挂载点表和 resolve |
| test_vfs_syscall | 28 个 kernel 测试，覆盖 syscall 全链路 |
| 分层测试策略 | host 单元测试 → kernel 集成测试 → shell 手动验证 |
