# 系统调用实现与 Shell 命令

## 概览

前两篇文章把 ext2 驱动的内部机制拆解完了。但用户态程序要用这些能力，还得经过系统调用这一层。这篇我们补上最后一块拼图：`sys_creat`、`sys_mkdir`、`sys_unlink`、`sys_rmdir` 四个系统调用的实现，`InodeOps` 虚表如何连接 VFS 和 ext2，以及 Shell 中 `touch`、`mkdir`、`rm`、`rmdir`、`echo` 的实现。最后回顾一个经典 bug——syscall 入口栈未对齐导致的 GP Fault。

## 架构图

```
用户态 Shell 命令
  touch /hello.txt   → sys_creat()     → VFS resolve → InodeOps::create
  mkdir /mydir        → sys_mkdir()     → VFS resolve → InodeOps::mkdir
  rm /hello.txt       → sys_unlink()    → VFS resolve → InodeOps::unlink
  rmdir /mydir        → sys_rmdir()     → VFS resolve → InodeOps::unlink (with empty check)
  echo hello > /f.txt → sys_creat + sys_open + sys_write + sys_close

所有 syscall 共享的路径:
  resolve_user_path (cwd+path join)
    → vfs_resolve (mount table lookup)
    → split_pathname (parent + leaf)
    → fs->lookup(parent)
    → parent->ops->create/mkdir/unlink (虚函数)
```

## 代码精讲

### InodeOps 虚表扩展

VFS 层通过 `InodeOps` 虚表和具体文件系统对接。和之前只读版本相比，新增了 `create`、`mkdir`、`unlink` 三个虚函数：

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

基类有默认实现（返回 -1 / nullptr），所以 ramdisk 等不支持写的文件系统不需要任何改动——`RamdiskFileOps` 和 `RamdiskDirOps` 不覆写这些函数，调用时直接返回错误。

ext2 提供两个子类：`Ext2FileOps`（普通文件的 read/write）和 `Ext2DirOps`（目录的 readdir/create/mkdir/unlink）。系统调用的核心逻辑就是：找到父目录 Inode → `parent->ops->create/mkdir/unlink` → 走到 ext2 的具体实现。

### 通用路径拆分 split_pathname

四个 syscall 共用同一个路径拆分逻辑，各有一份放在匿名 namespace 中的相同实现：

```cpp
bool split_pathname(const char* path, char* parent_out, const char** name_out,
                    uint32_t* namelen_out) {
    uint32_t len = static_cast<uint32_t>(strlen(path));
    if (len == 0) { return false; }
    if (path[len - 1] == '/') { return false; }  // trailing slash ambiguous

    int32_t last_sep = -1;
    for (uint32_t i = 0; i < len; ++i) {
        if (path[i] == '/') { last_sep = static_cast<int32_t>(i); }
    }

    if (last_sep < 0) {
        parent_out[0] = '\0';  // no separator → parent is root
        *name_out     = path;
        *namelen_out  = len;
    } else {
        uint32_t parent_len = static_cast<uint32_t>(last_sep);
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';
        *name_out    = path + last_sep + 1;
        *namelen_out = len - parent_len - 1;
    }
    return *namelen_out != 0;
}
```

从右往左找最后一个 `/`，左边是父目录路径，右边是叶子名。没有 `/` 说明在根目录下操作，父路径为空。以 `/` 结尾的路径被拒绝——对 create/mkdir/unlink 有歧义。

### sys_creat 实现

`sys_creat` 对应 POSIX 的 `creat(path, mode)`，我们简化忽略了 mode 参数：

```cpp
int64_t sys_creat(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) { return -1; }

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) { return -1; }

    char        parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;
    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) { return -1; }

    cinux::fs::Inode* parent = fs->lookup(parent_buf);
    if (parent == nullptr || parent->ops == nullptr) { return -1; }

    cinux::fs::Inode* new_inode = parent->ops->create(parent, leaf_name, name_len);
    if (new_inode != nullptr) { return 0; }

    // create returned nullptr — file may already exist, truncate it
    cinux::fs::Inode* existing = fs->lookup(rel_path);
    if (existing != nullptr && existing->ops != nullptr && existing->size > 0) {
        existing->size = 0;
    }
    return (existing != nullptr) ? 0 : -1;
}
```

标准的五步模式：`resolve_user_path`（用户虚拟地址拷贝到内核，处理 cwd 前缀）→ `vfs_resolve`（挂载表查找属于哪个文件系统）→ `split_pathname`（拆分父目录和叶子名）→ `fs->lookup(parent_buf)`（逐级遍历找到父目录 Inode）→ `parent->ops->create()`（虚函数调用到 ext2）。

如果 `create` 返回 nullptr，按 POSIX 语义截断已有文件为 0 字节。我们的截断很简陋——只设 `size = 0` 没有释放数据块，教学内核里先凑合用。

### sys_mkdir 和 sys_unlink

这两个和 `sys_creat` 结构几乎一样，只是最后调的虚函数不同：

```cpp
// sys_mkdir — 最后一步:
cinux::fs::Inode* new_inode = parent->ops->mkdir(parent, leaf_name, name_len);
return (new_inode != nullptr) ? 0 : -1;

// sys_unlink — 最后一步:
int64_t result = parent->ops->unlink(parent, leaf_name, name_len);
return (result == 0) ? 0 : -1;
```

四个 syscall 的前四步完全相同（resolve → vfs_resolve → split → lookup），只有第五步调的虚函数不同。高度一致的模式意味着加新 syscall（比如 `rename`、`symlink`）只需要复制前四步、改最后一行。

### sys_rmdir 实现

`sys_rmdir` 比 `sys_unlink` 多了一步——调用 unlink 前验证目标是空目录：

```cpp
int64_t sys_rmdir(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Step 1-4: same as sys_unlink
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) { return -1; }
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) { return -1; }

    char        parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;
    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) { return -1; }
    cinux::fs::Inode* parent = fs->lookup(parent_buf);
    if (parent == nullptr || parent->ops == nullptr) { return -1; }

    // Step 5: Verify target is an empty directory
    cinux::fs::Inode* target = fs->lookup(rel_path);
    if (target == nullptr) { return -1; }
    if (target->type != cinux::fs::InodeType::Directory) { return -1; }
    if (target->ops != nullptr) {
        char    check_name[16];
        int64_t rc = target->ops->readdir(target, 2, check_name, sizeof(check_name));
        if (rc > 0) { return -1; }  // directory not empty
    }

    // Step 6: Call unlink
    int64_t result = parent->ops->unlink(parent, leaf_name, name_len);
    return (result == 0) ? 0 : -1;
}
```

空目录检查巧妙利用了 `readdir` 的接口：index 0 是 "."、index 1 是 ".."、index 2 开始才是实际内容。如果 `readdir(target, 2, ...)` 返回有内容，说明目录不空。

### Shell 命令实现

有了 syscall，Shell 命令就是薄薄一层包装。所有命令都用相同的模式：参数检查 → 调 syscall → 处理返回值。

`touch` 调 `sys_creat` 创建文件：

```cpp
void cmd_touch(int argc, char** argv) {
    if (argc < 2) { write_str("touch: missing file operand\n"); return; }
    const char* path   = argv[1];
    int64_t     result = sys_creat(path);
    if (result < 0) {
        write_str("touch: cannot create '");
        write_str(path);
        write_str("'\n");
    }
}
```

`mkdir`、`rm`、`rmdir` 的结构和 `touch` 完全一样，只是分别调用 `sys_mkdir`、`sys_unlink`、`sys_rmdir`，这里就不重复贴了。

`echo` 有个特别功能——输出重定向：

```cpp
void cmd_echo(int argc, char** argv) {
    // Search for '>' redirect token
    int redirect_idx = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], ">") == 0) { redirect_idx = i; break; }
    }

    if (redirect_idx > 0 && redirect_idx + 1 < argc) {
        const char* path = argv[redirect_idx + 1];
        int64_t creat_result = sys_creat(path);
        if (creat_result < 0) { /* error */ return; }

        int64_t fd = sys_open(path, 1);  // O_WRONLY
        if (fd < 0) { /* error */ return; }

        for (int i = 1; i < redirect_idx; ++i) {
            if (i > 1) { sys_write(static_cast<int>(fd), " ", 1); }
            sys_write(static_cast<int>(fd), argv[i], strlen(argv[i]));
        }
        sys_write(static_cast<int>(fd), "\n", 1);
        sys_close(static_cast<int>(fd));
        return;
    }

    // Normal mode: print to stdout
    for (int i = 1; i < argc; ++i) {
        if (i > 1) { sys_write(1, " ", 1); }
        write_str(argv[i]);
    }
    sys_write(1, "\n", 1);
}
```

`echo hello world > /msg.txt` 的 argv 是 `["echo", "hello", "world", ">", "/msg.txt"]`。找到 `>` 的位置后，左边的参数是文本内容，右边是目标路径。流程是 `sys_creat` → `sys_open` → `sys_write` → `sys_close`。注意 `>` 必须是独立参数（两边有空格），因为这是 echo 自己解析的，不是 Shell 解析器做的。

### Ramdisk 的写操作（或者说没有写操作）

ramdisk 的写操作直接返回 -1：

```cpp
int64_t RamdiskFileOps::write(Inode*, uint64_t, const void*, uint64_t) { return -1; }
int64_t RamdiskDirOps::write(Inode*, uint64_t, const void*, uint64_t) { return -1; }
```

ramdisk 也没有覆写 `create`、`mkdir`、`unlink`——走基类默认实现返回 -1。所有写操作只对 ext2 挂载点下的路径生效。

### 踩坑记录：syscall GP Fault

开发 `sys_creat` 时踩到了一个非常经典的对齐 bug。在 QEMU 中执行 `touch /hello2.txt` 触发 GP Fault：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  ERROR CODE = 0x0000000000000000
========================================
```

通过 `nm` 反查，RIP 落在 `Ext2::create()` 内部。反汇编发现崩溃指令是 `movaps %xmm0,(%rax)`——SSE 的 `movaps` 要求 16 字节对齐，而 RAX 不是 16 的倍数。

根源不在 `Ext2::create`，而在 syscall 汇编入口。`syscall_entry` push 了 12 个寄存器（96 字节）再 push 第 7 个 C 参数（8 字节），共 104 字节。104 % 16 = 8，导致 `call syscall_dispatch` 前 RSP 不是 16 的倍数，违反 SysV AMD64 ABI 的栈对齐约定。编译器在 `Ext2::create` 中生成 `movaps` 来零初始化栈上的 `Ext2Inode`，就触发了 GP fault。

修复方法：在 push 第 7 个参数前加 `subq $8, %rsp` 插入 padding，使 RSP 回到 16 字节对齐。所有 trap frame 内偏移量相应调整（+8 变为 +16），清理时 `addq` 从 $8 改为 $16。

这个 bug 之所以隐蔽，是因为之前的 syscall（`sys_read`、`sys_write`、`sys_open`）执行路径上恰好没触发 SSE 对齐指令。`sys_creat` 是第一个深入调用 ext2 复杂逻辑的 syscall，编译器才首次生成 `movaps`。这就是"一直存在但不一定触发"的 latent bug，排查方法详见 `document/notes/028b/syscall_gp_fault.md`。

## 设计决策

每个写操作 syscall 自行拆分路径、查找父目录、调用 InodeOps。路径解析会被执行多次（比如 `sys_rmdir` 中 lookup target 和 unlink 内部再 lookup parent），而 Linux 内核会合并成一次 path walk 并配合 dcache。教学内核里代码清晰度比性能更重要，所以选择了这种直接的模式。

`split_pathname` 被复制了四份，不够 DRY。更好的做法是提取到公共头文件，但目前每个 syscall 文件自包含也有简单性的优势。

Shell 的 `echo` 内置了输出重定向，而不是在 Shell 解析层统一处理。这意味着 `>` 只对 echo 有效，且必须两边有空格。更完整的设计是在 Shell 层实现通用 I/O 重定向（类似 `fork + dup2 + exec`），但那需要进程管理的支持。

## 扩展方向

接下来可以实现 `sys_rename`（移动/重命名文件）、`sys_truncate`（正确截断并释放数据块）、Shell 层通用 I/O 重定向（支持 `<` 和 `>>`）、`sys_chmod`（修改文件权限）。syscall 入口栈对齐的教训值得延伸：所有手写汇编入口（interrupt handler、exception handler）都应做一次统一的 16 字节对齐审计。

## 参考资料

- POSIX.1-2017: `creat()`, `mkdir()`, `unlink()`, `rmdir()` semantics
- System V ABI AMD64 Supplement: Stack alignment requirements (RSP must be 16-byte aligned before `call`)
- Intel SDM Vol. 2A: `movaps` — requires 16-byte alignment, raises #GP on misalignment
- Linux kernel: `fs/namei.c` (VFS path lookup), `fs/open.c` (sys_creat)
- Cinux debug notes: `document/notes/028b/syscall_gp_fault.md`
