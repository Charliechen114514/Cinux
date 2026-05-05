# M3: UID/GID + 文件权限

> 多用户基础。进程凭证（uid/gid/euid/egid）和文件权限检查。
> 为 chmod/chown/su 等命令提供基础。

## 任务清单

### T1: 进程凭证

**文件**: `kernel/proc/process.hpp`

```cpp
struct Task {
    // ... existing ...
    uint32_t uid, gid;       // 真实 UID/GID
    uint32_t euid, egid;     // 有效 UID/GID
    uint32_t suid, sgid;     // 保存的 setuid/setgid
};
```

- [ ] Task 添加凭证字段
- [ ] init 进程：uid=0, gid=0 (root)
- [ ] fork 继承父进程凭证
- [ ] execve(setuid 程序) 时设置 euid

### T2: 文件权限检查

**文件**: `kernel/fs/inode.hpp` — Inode 已有 mode/uid/gid 字段

```cpp
// 检查当前进程是否有权访问 inode
bool check_permission(const Inode* inode, int access_mode);
// access_mode: R_OK=4, W_OK=2, X_OK=1
```

- [ ] 权限检查逻辑（owner/group/other 三组 rwx）
- [ ] root 用户绕过检查 (uid == 0)
- [ ] VFS 操作前调用 check_permission

### T3: Syscall

| Syscall | 说明 |
|---------|------|
| chmod | 修改文件权限 |
| chown | 修改文件所有者 |
| getuid/getgid | 获取真实 UID/GID |
| geteuid/getegid | 获取有效 UID/GID |
| setuid/setgid | 设置 UID/GID |

- [ ] chmod(path, mode) — 只有 owner 或 root 可修改
- [ ] chown(path, uid, gid) — 只有 root 可修改
- [ ] uid/gid 查询和设置 syscall

## 产出物

- [ ] Task 凭证字段
- [ ] 文件权限检查
- [ ] chmod/chown/getuid/setuid syscall
