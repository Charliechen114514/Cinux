# F10: 用户态运行时

> 完善用户态生态：libc 扩展、动态链接器、TTY 子系统、CFBox + init。

## 实现决策

全部四项：
1. libc 扩展（21→80 syscall）
2. ELF 动态链接器
3. TTY 子系统（伪终端 + 行规范）
4. CFBox 集成 + init 系统

## Milestone 依赖

```
M1 libc 扩展 ──→ M2 ELF 动态链接器
       ↓                ↓
M3 TTY 子系统    M4 CFBox + init
                        ↓
                 M5 musl libc + glibc 兼容验证
```

M1 是所有其他的前置。M3 可与 M2 并行。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-libc.md](00-libc.md) | M1: libc 扩展到 80 syscall |
| [01-elf-dynamic.md](01-elf-dynamic.md) | M2: ELF 动态链接器 |
| [02-tty.md](02-tty.md) | M3: TTY 子系统 |
| [03-cfbox-init.md](03-cfbox-init.md) | M4: CFBox + init |
| [04-musl-glibc.md](04-musl-glibc.md) | M5: musl libc + glibc 兼容 |
