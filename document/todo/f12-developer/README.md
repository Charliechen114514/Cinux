# F12: 开发者生态

> 调试工具、Lua 脚本、TinyCC 编译器、编辑器和包管理器。
> 让 Cinux 成为可自举的开发平台。

## 实现决策

全部四项：
1. 调试工具（GDB stub + KALLSYMS + panic 增强）
2. Lua 5.4 脚本
3. TinyCC 编译器
4. 文本编辑器 + 包管理器

## Milestone 依赖

```
M1 调试工具（独立，内核侧）
M2 Lua 5.4（用户态，需 libc）
M3 TinyCC（用户态，需 libc + 文件系统）
M4 编辑器 + 包管理器（用户态，需前面全部）
```

按顺序推进。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-debug.md](00-debug.md) | M1: GDB stub + KALLSYMS |
| [01-lua.md](01-lua.md) | M2: Lua 5.4 |
| [02-tinycc.md](02-tinycc.md) | M3: TinyCC 编译器 |
| [03-tools.md](03-tools.md) | M4: 编辑器 + 包管理器 |
