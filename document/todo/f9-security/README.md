# F9: 安全机制

> 四层安全增强：NX/SMEP/SMAP → ASLR → UID/GID → Stack Canary。
> 按防护效果和依赖关系排序。

## 实现决策

| 优先级 | 机制 | 说明 |
|--------|------|------|
| M1 | NX + SMEP/SMAP | CPU 硬件级保护，代码量小，效果最大 |
| M2 | ASLR | 地址空间随机化，需要随机数源 |
| M3 | UID/GID + 文件权限 | 多用户基础，chmod/chown |
| M4 | Stack Canary | 编译器 flag 配置 |

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-nx-smep.md](00-nx-smep.md) | M1: NX + SMEP/SMAP |
| [01-aslr.md](01-aslr.md) | M2: ASLR |
| [02-uid-gid.md](02-uid-gid.md) | M3: UID/GID + 文件权限 |
| [03-canary.md](03-canary.md) | M4: Stack Canary |
