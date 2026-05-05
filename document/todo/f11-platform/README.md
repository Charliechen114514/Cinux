# F11: 启动与平台

> UEFI 启动支持 + FAT32 文件系统。
> 真实硬件适配推到后续（需要物理机测试环境）。

## 实现决策

| 内容 | 说明 |
|------|------|
| UEFI 启动 | GOP framebuffer + UEFI 内存映射 + Boot Services |
| FAT32 | UEFI ESP 分区需要 + U 盘互操作 |
| 真实硬件 | 暂不做（需要物理机） |

## Milestone 依赖

```
M1 FAT32 ──→ M2 UEFI 启动（ESP 需要 FAT32）
```

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-fat32.md](00-fat32.md) | M1: FAT32 文件系统 |
| [01-uefi.md](01-uefi.md) | M2: UEFI 启动支持 |
