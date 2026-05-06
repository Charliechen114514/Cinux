# 027-2 Read-through: 挂载点表与内核字符串库 — VFS 的路由层

## 概览

本文讲解 VFS 挂载点表的完整实现（`vfs_mount.hpp` 和 `vfs_mount.cpp`）以及内核 freestanding 字符串库（`string.hpp` 和 `string.cpp`）。挂载点表是 VFS 的"路由器"——它把文件路径前缀映射到具体的 FileSystem 后端，路径解析时做最长前缀匹配。字符串库则为挂载点表的路径操作提供基础设施。同时我们还会看到 g_global_fd_table 全局访问器和 __cxa_guard 支持。

关键设计决策：使用固定大小数组（8 槽位）而非动态容器；最长前缀匹配而非精确匹配；路径边界检查防止 "/mnt" 误匹配 "/mnt2"。

## 架构图

```
  sys_open("/mnt/disk/file.txt")
        │
        ▼
  vfs_resolve()
  ┌──────────────────────────────┐
  │ Mount Table (8 slots)        │
  │ [0] "/"     → Ramdisk       │  prefix "/" 长度 1
  │ [1] "/mnt"  → ext2          │  prefix "/mnt" 长度 4 ← 最长匹配!
  │ [2] unused                  │
  │ ...                         │
  └──────────────────────────────┘
        │
        ▼ best_len=4, rel_path="disk/file.txt"
  ext2->lookup("disk/file.txt")
```

## 代码精讲

### 内核字符串库声明

```cpp
extern "C" {
void*  memset(void* dest, int val, size_t count);
void*  memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);
void*  memmove(void* dest, const void* src, size_t count);
int    memcmp(const void* a, const void* b, size_t count);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
size_t strlen(const char* s);
char*  strcpy(char* __restrict__ dest, const char* __restrict__ src);
int    utoa(char* buf, uint32_t value);
}
```

所有函数用 extern "C" 包裹，匹配标准 libc 签名。这样做有两个好处：C++ 代码可以正常调用（因为头文件声明在 extern "C" 里，链接器会找 C 链接名的符号），而且如果以后有纯 C 的内核代码也能链接到这些函数。__restrict__ 是 GCC 扩展关键字，告诉编译器 dest 和 src 指向的内存不重叠，允许更激进的优化。除了 7 个标准函数外，还提供了 strcpy（字符串拷贝）和 utoa（无符号整数转字符串）——前者是路径操作的基础，后者用于 kprintf 等需要数字转字符串的场景。

### memmove — 处理重叠拷贝

```cpp
void* memmove(void* dest, const void* src, size_t count) {
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; ++i) { d[i] = s[i]; }
    } else if (d > s) {
        for (size_t i = count; i > 0; --i) { d[i - 1] = s[i - 1]; }
    }
    return dest;
}
```

memmove 是七个函数里唯一需要特别处理的——当 dest 和 src 有重叠区域时，拷贝方向决定了正确性。如果 dest > src 且正向拷贝，后面的源数据会被已写入的数据覆盖——所以必须反向拷贝。如果 dest < src，正向拷贝是安全的。如果 dest == src，什么都不做（两个分支都不满足），直接返回。这个实现虽然朴素但完全正确，Linux 内核在早期版本也是这么做的。

### MountPoint 结构和常量

```cpp
static constexpr uint32_t MOUNT_TABLE_SIZE = 8;
static constexpr uint32_t MOUNT_PATH_MAX = 256;

struct MountPoint {
    char        path[MOUNT_PATH_MAX];
    FileSystem* fs;
    bool        in_use;
};
```

MOUNT_TABLE_SIZE = 8 对教学 OS 来说足够了——Linux 桌面系统通常也只有十几个挂载点。path 用固定大小数组而非动态分配，避免了堆分配的复杂性。in_use 标志替代了从数组中删除元素的需要——删除只需置 false，添加时找第一个 in_use=false 的槽位。

### vfs_mount_init 和 vfs_mount_add

所有挂载点表函数都在 `cinux::fs` 命名空间内。挂载点表本身是一个 static 全局数组 `g_mount_table`，还有一个 `g_mount_lock` 自旋锁保护并发访问。

```cpp
void vfs_mount_init() {
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        g_mount_table[i].path[0] = '\0';
        g_mount_table[i].fs      = nullptr;
        g_mount_table[i].in_use  = false;
    }
}
```

init 把所有槽位标记为未使用。这在内核启动时调用一次——如果忘了调用，后续的 add 和 resolve 行为是未定义的（in_use 可能是内存中的垃圾值 true）。

```cpp
bool vfs_mount_add(const char* path, FileSystem* fs) {
    if (path == nullptr || fs == nullptr) return false;

    auto g = g_mount_lock.guard();
    (void)g;

    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use) {
            uint32_t len = 0;
            while (path[len] != '\0') { ++len; }
            if (len == 0 || len >= MOUNT_PATH_MAX) return false;
            memcpy(g_mount_table[i].path, path, len + 1);
            g_mount_table[i].fs     = fs;
            g_mount_table[i].in_use = true;
            return true;
        }
    }
    return false;
}
```

add 的流程很直观：先获取自旋锁保护，然后找空位、算长度、检查合法性、拷贝路径、设置指针、标记占用。注意 memcpy 的第三个参数是 len + 1——包含 null 终止符。如果 8 个槽位都满了，返回 false。

### vfs_resolve — 最长前缀匹配的核心

```cpp
FileSystem* vfs_resolve(const char* path, const char** rel_path) {
    if (path == nullptr || rel_path == nullptr) return nullptr;

    auto g = g_mount_lock.guard();
    (void)g;

    FileSystem* best_fs   = nullptr;
    uint32_t    best_len  = 0;

    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use) continue;
        const char* mpath = g_mount_table[i].path;
        uint32_t mlen = 0;
        while (mpath[mlen] != '\0') { ++mlen; }

        if (strncmp(mpath, path, mlen) != 0) continue;

        char last_mount_char = mpath[mlen - 1];
        if (last_mount_char != '/') {
            if (path[mlen] != '\0' && path[mlen] != '/') continue;
        }

        if (mlen > best_len) {
            best_len = mlen;
            best_fs  = g_mount_table[i].fs;
        }
    }

    if (best_fs != nullptr) {
        *rel_path = path + best_len;
    }
    return best_fs;
}
```

这段代码有几个值得细看的地方。首先，strncmp 只比较前 mlen 个字符——如果挂载路径是 "/mnt"，那 path 前 4 个字符必须是 "/mnt" 才能通过第一轮筛选。然后做路径边界检查：挂载路径的最后一个字符如果不是 '/'，那 path 中紧接前缀的下一个字符必须是 '\0' 或 '/'。这个检查防止了 "/mnt" 匹配 "/mnt2/file" 的错误。最后，只在 mlen > best_len 时更新——确保返回的是最长前缀匹配。

当 "/" 挂载 Ramdisk 时，mpath = "/"，mlen = 1。对于 path = "/hello.txt"，strncmp("/", "/hello.txt", 1) 比较 '/' 和 '/' 匹配成功。last_mount_char = '/'，跳过边界检查。best_len = 1，rel_path 指向 "hello.txt"。完美。

### g_global_fd_table、current_fd_table 和 __cxa_guard

```cpp
FDTable& g_global_fd_table() {
    static FDTable s_global_fd_table;
    return s_global_fd_table;
}

FDTable& current_fd_table() {
#ifndef CINUX_HOST_TEST
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr && task->fd_table != nullptr) {
        return *task->fd_table;
    }
#endif
    return g_global_fd_table();
}
```

g_global_fd_table 使用函数内 static 局部变量实现延迟初始化——第一次调用时构造，之后每次返回同一个引用。current_fd_table 是更高级的访问器——它先尝试获取当前任务的 per-process FDTable（在多进程环境中每个进程有自己的 fd 表），如果不存在就回退到全局 FDTable。在 milestone 027 阶段，所有任务都使用全局 FDTable，但接口已经为将来的 per-process 隔离做好了准备。

函数内的 static 局部变量在 C++ 中保证只初始化一次——编译器通过 __cxa_guard_acquire/release 机制实现这一点。在单核内核中，guard 的实现非常简单：

```cpp
int __cxa_guard_acquire(uint64_t* guard) {
    if (*guard != 0) {
        return 0;
    }
    return 1;
}
void __cxa_guard_release(uint64_t* guard) {
    *guard = 1;
}
```

guard 变量初始为 0（BSS 段自动清零），acquire 检查是否为 0，是则返回 1（需要初始化），release 将其设为 1（已初始化）。在多核环境下这不安全（没有原子操作），但我们的单核内核不需要担心。

## 设计决策

### 决策：固定数组 vs 动态容器

**问题**: 挂载点表用什么数据结构？

**本项目的做法**: 固定大小数组（8 个 MountPoint）。

**备选方案**: 使用链表或红黑树动态管理挂载点。

**为什么不选备选方案**: 内核启动时挂载点数量确定且很少（通常 1-3 个），固定数组最简单、最可预测。不需要动态分配内存，不需要处理分配失败的情况。resolve 时遍历 8 个元素比遍历链表或树还快（缓存友好）。

**如果要扩展**: 将来如果需要支持大量挂载点，可以把数组改为哈希表（按路径首字母分桶）或者 RCU 保护的链表。

## 扩展方向

- 实现路径规范化——处理 ".." 和 "." 以及多余的 "/"（难度：⭐⭐）
- 支持挂载选项（只读/读写/同步/异步）（难度：⭐）
- 添加 vfs_mount_replace()——替换已有挂载点而不需要先 remove（难度：⭐）
- 使用 Trie 数据结构加速最长前缀匹配（难度：⭐⭐⭐）

## 参考资料

- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — Mount Point List 模型和路径解析策略
- Linux Kernel: [Overview of the VFS](https://docs.kernel.org/filesystems/vfs.html) — vfsmount 树形结构
- OSDev Wiki: [Hierarchical VFS Theory](https://wiki.osdev.org/Hierarchical_VFS_Theory) — 目录树 vs 挂载点列表的对比
