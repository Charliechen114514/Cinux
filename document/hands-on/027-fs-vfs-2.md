# 027-2 Hands-on: 挂载点表与路径解析 — VFS 的路由器

## 导语

上一章我们搭好了 VFS 的数据结构地基：Inode、InodeOps、FileSystem 基类、File、FDTable。但光有这些还不够——当 sys_open("/hello.txt") 被调用时，内核怎么知道应该去找 Ramdisk 还是 ext2？答案是挂载点表（Mount Table），它就像一个路由器：把文件路径前缀映射到对应的文件系统后端，路径解析时做最长前缀匹配，找到正确的后端后把剩余的相对路径交给它处理。

同时，这一章我们还要实现内核自己的字符串函数库（memset/memcpy/strcmp 等），因为挂载点表和路径解析需要大量字符串操作，而 freestanding 环境下没有标准 libc 可用。完成本章后，我们就能把一个 Ramdisk 实例注册到挂载点表里，通过 vfs_resolve 把任意路径路由到正确的文件系统后端。

知识前置：需要完成 027-1（VFS 数据结构设计）和 026（Ramdisk）。

## 概念精讲

### 挂载点表 — VFS 的路由器

挂载点表的核心思路用一句话就能说清：把一个路径前缀绑定到一个 FileSystem 实例。比如把 "/" 绑定到 Ramdisk、把 "/mnt/disk" 绑定到 ext2——当用户调用 open("/mnt/disk/readme.txt") 时，VFS 先在挂载点表里找最长的匹配前缀 "/mnt/disk"，然后把剩余的相对路径 "readme.txt" 交给 ext2 的 lookup() 处理。

这种"最长前缀匹配"的设计和 Linux 的 mount 命令行为一致：如果 "/" 和 "/mnt/disk" 都有挂载，那 "/mnt/disk/file" 会匹配到 "/mnt/disk" 而不是 "/"，因为前者前缀更长、更精确。OSDev Wiki 把这种设计叫做"Mount Point List"模型——简单、直观，虽然在大规模挂载时性能不如 Node Graph，但对教学 OS 来说绰绰有余。

### 路径边界检查 — 最容易被忽略的细节

最长前缀匹配有一个微妙的问题：如果挂载了 "/mnt"，那 "/mntdata/file" 不应该匹配到 "/mnt"。我们需要确保前缀匹配发生在路径组件边界上——也就是说，匹配前缀的下一个字符必须是 '/' 或者 '\0'（字符串结束），又或者挂载路径本身以 '/' 结尾（比如 "/" 挂载点，任何以 "/" 开头的路径都匹配）。

### 内核字符串库 — freestanding 的必修课

在 freestanding 环境下（没有标准库），你必须自己实现 memset、memcpy、memmove、memcmp、strcmp、strncmp、strlen 这些最基础的函数。它们都放在 `kernel/lib/string.hpp` 和 `string.cpp` 里，用 extern "C" 链接，这样既能被 C++ 代码直接调用，也能被可能存在的 C 代码链接。memmove 需要处理源和目标重叠的情况——当目标地址小于源地址时正向拷贝，反之则反向拷贝。

## 动手实现

### Step 1: 实现内核字符串函数库

**目标**: 创建 `kernel/lib/string.hpp` 和 `kernel/lib/string.cpp`，实现 7 个基础函数。

**设计思路**: 所有函数声明在 extern "C" 块内，匹配标准 libc 签名。memset 逐字节填充；memcpy 逐字节拷贝（用 __restrict__ 告诉编译器 src 和 dest 不重叠）；memmove 先判断地址大小关系再决定拷贝方向；memcmp 逐字节比较；strcmp 和 strncmp 逐字符比较到 '\0'；strlen 遍历到 '\0' 计数。

**实现约束**: 不要试图用 SIMD 优化这些函数——对于内核来说，简单的逐字节实现就够了，可读性和正确性远比性能重要。memcpy 的 __restrict__ 是 GCC 扩展，告诉编译器 src 和 dest 指向的内存不重叠，允许更好的优化。

**踩坑预警**: memmove 是唯一一个需要正确处理重叠的函数。如果 dest > src 且有重叠区域，正向拷贝会覆盖还没读到的数据——必须反向拷贝。如果 dest < src，正向拷贝是安全的。如果 dest == src，什么都不做直接返回。

**验证**: 编译通过后，可以在 host 测试中验证——创建 test 文件调用 memset/memcpy/memcmp 等，确认返回值和内存状态符合预期。

### Step 2: 定义 MountPoint 结构和挂载点表接口

**目标**: 创建 `kernel/fs/vfs_mount.hpp`，定义挂载点表的数据结构和操作函数。

**设计思路**: MountPoint 结构包含三个字段——path（char 数组，256 字节，存储挂载路径如 "/" 或 "/mnt"）、fs（FileSystem 指针，指向具体的文件系统后端实例）、in_use（bool 标志，表示这个槽位是否被占用）。挂载点表是全局的固定大小数组（8 个槽位，MOUNT_TABLE_SIZE = 8），对教学 OS 来说足够了。

**实现约束**: 四个操作函数——vfs_mount_init()（清空所有槽位）、vfs_mount_add(path, fs)（找到空槽位、拷贝路径、设置 fs 和 in_use）、vfs_mount_remove(path)（找到匹配路径的槽位、置 in_use=false）、vfs_resolve(path, &rel_path)（最长前缀匹配、返回 FileSystem* 并设置 rel_path 指向剩余路径）。

**踩坑预警**: vfs_mount_add 需要手动计算路径长度（不能用 strlen，因为此时可能还在初始化阶段）——实际上我们已经有了自己实现的 strlen，可以直接用。路径长度不能超过 MOUNT_PATH_MAX（256），否则返回 false。

**验证**: 编译通过。

### Step 3: 实现 vfs_resolve — 最长前缀匹配

**目标**: 在 `kernel/fs/vfs_mount.cpp` 中实现 vfs_resolve 函数。

**设计思路**: 遍历挂载点表的所有 in-use 条目，对每个条目用 strncmp 检查路径是否以挂载路径开头。如果匹配，再检查路径边界——挂载路径最后一个字符是 '/' 的话直接算匹配成功，否则要求路径中紧接前缀的下一个字符是 '/' 或 '\0'。记录最长匹配的条目，最终返回其 FileSystem 指针，并设置 rel_path 指向 path + 前缀长度。

**实现约束**: rel_path 是输出参数——如果找到匹配，*rel_path 被设为 path + best_len（指向剥离前缀后的剩余路径）。如果没找到任何匹配，返回 nullptr。比如 "/" 挂载 Ramdisk，解析 "/hello.txt" 时，rel_path 指向 "hello.txt"。

**踩坑预警**: 当挂载路径是 "/" 时，best_len = 1，rel_path 指向 "hello.txt"——这是正确的。但如果挂载路径是 "/mnt"（没有尾随斜杠），解析 "/mnt/file" 时 best_len = 4，rel_path 应该指向 "/file"——等等，这不对！实际上 rel_path 应该指向挂载点之后的路径部分。如果挂载路径是 "/mnt"，解析 "/mnt/file" 时，剥离 "/mnt" 后 rel_path 指向 "/file"，这时候 ext2 的 lookup 需要能处理前导 '/'。Cinux 的做法是让 Ramdisk 的 lookup 在比较前先跳过前导 '/'——简单粗暴但管用。

**验证**:
```bash
cd build && cmake .. && make test_vfs_mount
ctest -R vfs_mount --output-on-failure
```
预期：19 个测试全部通过，包括最长前缀匹配测试。

### Step 4: 实现 g_global_fd_table 全局访问器

**目标**: 在 vfs_mount.cpp 中实现一个返回全局 FDTable 引用的函数。

**设计思路**: 在 milestone 027 中，我们只有一个全局的 FDTable（单进程内核）。使用函数内的 static 局部变量实现延迟初始化——第一次调用时构造，之后每次返回同一个引用。这种方式需要 __cxa_guard_acquire/release 的支持（我们已经在 crt_stub.cpp 中实现了）。

**实现约束**: 返回类型是 FDTable&（引用），函数名 g_global_fd_table()。在 crt_stub.cpp 中需要添加 __cxa_guard_acquire 和 __cxa_guard_release 的实现——这两个函数被编译器用来保证函数内 static 变量的线程安全初始化。在单核内核中，只需要检查 guard 变量是否为 0（未初始化）即可。

**踩坑预警**: 如果没有实现 __cxa_guard_acquire/release，链接时会报 undefined reference 错误。这两个函数是 C++ ABI 的一部分，GCC 在编译函数内 static 变量时会自动生成对它们的调用。在 freestanding 环境下必须手动提供。

**验证**: 编译通过。g_global_fd_table() 多次调用应返回同一个对象的地址。

### Step 5: 更新 kernel/main.cpp — 初始化 VFS

**目标**: 在 kernel_main() 中添加 VFS 初始化代码。

**设计思路**: 把 Ramdisk 实例从局部变量改为 static（避免栈溢出，而且生命周期需要覆盖整个运行期），然后依次调用 vfs_mount_init() 和 vfs_mount_add("/", &ramdisk) 把 Ramdisk 注册到挂载点表的根路径。

**实现约束**: Ramdisk 的 mount() 现在返回 bool 而非 uint32_t，需要检查返回值。如果 mount 失败，打印错误信息但不 panic——内核仍然可以运行（只是没有文件系统可用）。

**验证**:
```bash
cd build && cmake .. && make big_kernel && qemu-system-x86_64 -serial stdio -kernel big_kernel.bin 2>&1 | grep -E "(VFS|RAMDISK|Milestone)"
```
预期看到 "[VFS] Ramdisk mounted at /" 和之前一样的 Ramdisk 文件列表输出。

## 构建与运行

```bash
cd build && cmake .. && make test_host
```

预期：所有 host 测试通过，包括新增的 test_vfs_mount（19 个用例）。QEMU 内核测试中会看到 VFS 初始化的串口输出。

## 调试技巧

**vfs_resolve 返回 nullptr**: 检查是否调用了 vfs_mount_init()——如果表没初始化，所有槽位的 in_use 都是未定义的垃圾值，resolve 可能匹配到错误条目或直接崩溃。确保 init 在 add 之前调用。

**路径匹配到了错误的文件系统**: 最常见的原因是忘记做路径边界检查。比如挂载了 "/mnt" 后，"/mnt2/file" 也被匹配到了 "/mnt"。检查 strncmp 的长度参数和后续的边界字符判断是否正确。

**__cxa_guard_acquire 链接失败**: 确认 kernel/arch/x86_64/crt_stub.cpp 中有这两个函数的实现。它们必须在 extern "C" 链接下，因为编译器生成的调用使用的是 C 链接名。

## 本章小结

| 概念 | 要点 |
|------|------|
| MountPoint | 路径前缀 + FileSystem 指针 + in_use 标志 |
| 挂载点表 | 固定 8 槽位的全局数组 |
| vfs_mount_init/add/remove | 初始化、注册、注销挂载点 |
| vfs_resolve | 最长前缀匹配 + 路径边界检查，返回 FileSystem* 和相对路径 |
| 路径边界 | 前缀后的字符必须是 '/' 或 '\0'，或者前缀以 '/' 结尾 |
| g_global_fd_table | 返回全局 FDTable 引用，static 延迟初始化 |
| string.cpp | 7 个 freestanding 字符串/内存函数，extern "C" 链接 |
