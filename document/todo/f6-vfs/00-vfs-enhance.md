# M1: VFS 增强 — Dentry Cache + 链接 + 文件锁

> 三项 VFS 层增强：目录项缓存加速路径解析、符号/硬链接支持、文件锁。

## 目标

提升 VFS 性能和 POSIX 兼容性。

## 任务清单

### T1: Dentry Cache

**文件**: `kernel/fs/dentry.hpp`, `kernel/fs/dentry.cpp`

缓存路径解析结果，避免每次 lookup 都遍历磁盘目录：

```cpp
struct Dentry {
    char        name[256];      // 文件/目录名
    Dentry*     parent;         // 父 dentry
    Inode*      inode;          // 关联 inode
    FileSystem* fs;             // 所属文件系统
    bool        valid;          // 是否有效（inode 可能被删除）

    // 子 dentry 哈希表
    Dentry*     children[DENTRY_HASH_BUCKETS];
    Dentry*     hash_next;      // 同桶链

    // LRU 链
    Dentry*     lru_prev;
    Dentry*     lru_next;
};

class DentryCache {
public:
    // 查找
    Dentry* lookup(Dentry* parent, const char* name);

    // 添加
    Dentry* add(Dentry* parent, const char* name, Inode* inode);

    // 失效（文件删除/重命名时）
    void invalidate(Dentry* dentry);

    // LRU 淘汰
    void shrink(size_t target_count);

    size_t count() const;
};
```

- [ ] Dentry 结构体
- [ ] DentryCache 哈希查找
- [ ] lookup() — 命中返回，未命中返回 nullptr
- [ ] add() — 新建 dentry 并插入
- [ ] invalidate() — 删除时标记无效
- [ ] LRU 淘汰（简单双向链表）
- [ ] 集成到 vfs_resolve() 路径解析

### T2: 符号链接支持

**文件**: `kernel/fs/inode.hpp`（扩展）

- [ ] InodeType 增加 `Symlink = 3`
- [ ] InodeOps 增加 `readlink()` 虚方法
- [ ] 路径解析遇到 symlink → 读取目标 → 重新解析
- [ ] 循环检测（最大 8 层 symlink 跟随）
- [ ] symlink() syscall — 创建符号链接
- [ ] readlink() syscall — 读取链接目标
- [ ] lstat() syscall — 不跟随链接的 stat

### T3: 硬链接支持

**文件**: `kernel/fs/inode.hpp`（扩展）

- [ ] InodeOps 增加 `link()` 虚方法
- [ ] Inode 的 nlink 字段正确维护
- [ ] link() syscall — 创建硬链接
- [ ] unlink() 减少 nlink，nlink=0 时删除文件

### T4: 文件锁（flock）

**文件**: `kernel/fs/file_lock.hpp`, `kernel/fs/file_lock.cpp`

```cpp
enum class LockType {
    Shared,     // LOCK_SH — 共享锁（读）
    Exclusive,  // LOCK_EX — 独占锁（写）
    Unlock,     // LOCK_UN — 解锁
};

class FileLockManager {
public:
    // flock 操作
    int flock(File* file, LockType type, bool nonblock);

private:
    struct LockEntry {
        Inode*   inode;
        Task*    owner;
        LockType type;
        LockEntry* next;
    };
    LockEntry* locks_;  // 全局锁链表
};
```

- [ ] FileLockManager 实现
- [ ] 共享锁：多个读者可同时持有
- [ ] 独占锁：只有一个持有者
- [ ] 阻塞等待（非阻塞模式返回 EWOULDBLOCK）
- [ ] flock() syscall（Linux syscall 73）
- [ ] 文件关闭时自动释放锁

### T5: mount / umount syscall

```cpp
int64_t sys_mount(const char* source, const char* target,
                  const char* fstype, uint64_t flags, const void* data);
int64_t sys_umount(const char* target);
int64_t sys_umount2(const char* target, int flags);
```

- [ ] sys_mount — 挂载文件系统到目标路径
  - 查找 fstype 对应的 FileSystem 实现（ext2/procfs/devfs/tmpfs/fat）
  - 调用 vfs_mount_add(target, fs)
- [ ] sys_umount2 — 卸载（检查是否有进程使用）
- [ ] 支持的 fstype：ext2, ext4, proc, devfs, tmpfs, ramfs, fat
- [ ] mount flags：MS_RDONLY, MS_NOSUID
- [ ] libc mount/umount wrapper

### T6: 单元测试

- [ ] DentryCache 命中/未命中
- [ ] 符号链接解析（多层跟随）
- [ ] 硬链接 nlink 正确
- [ ] flock 共享/独占互斥
- [ ] 路径解析使用 dentry cache 加速

## 产出物

- [ ] `kernel/fs/dentry.hpp` / `.cpp` — Dentry Cache
- [ ] `kernel/fs/file_lock.hpp` / `.cpp` — 文件锁
- [ ] symlink/readlink/link/lstat/flock syscall
- [ ] InodeOps 扩展
- [ ] 路径解析集成 dentry cache
