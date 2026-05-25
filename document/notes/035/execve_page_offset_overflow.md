---
title: execve 页内偏移越界
---

# 035 execve ELF 加载页内偏移越界：shell 只输出用户输入，不执行命令

## 现象

GUI 终端里 shell 能实时回显按键（按 `p` 显示 `p`），但敲回车后：
- 不显示命令执行结果（`pwd` 应该输出 `/`）
- 终端上只看到用户刚敲的那串字符（如 `pwd`、`ls`）
- 没有欢迎信息 `Cinux shell - type 'help' for commands`
- 没有提示符 `cinux> `

## 初步排查

### 第一步：加日志看 pipe 数据流

在 `sys_write`、`Pipe::write`、`poll_output` 三处加了数据内容日志：

```
[SYS_WRITE] fd=1 file=... inode=... ops=...           ← 有
[PIPE_WRITE] 1 bytes, count_=1: "p"                    ← 有
[SYS_WRITE_DATA] 1 bytes: "p"                          ← 有
```

单字符 echo 正常。但 shell 启动时的 welcome/prompt 写入：

```
[SYS_WRITE] fd=1 ... ops=...                           ← 有（ops 非空）
（无 PIPE_WRITE）                                        ← 没有！数据没进 pipe
```

`sys_write` 走的是 VFS 路径（ops 非空），但 `Pipe::write` 没被调用。

### 第二步：发现 `\x00` 替换 `\n`

日志中 `\n` echo 写进了 pipe，但内容是 `\x00`：

```
[PIPE_WRITE] 1 bytes: "\x00"     ← shell 写 "\n"，pipe 里变成 \x00
```

### 第三步：识别出 stack vs .rodata 的差异

对比哪些写成功、哪些失败：

| 来源 | 地址类型 | 结果 |
|------|----------|------|
| `write_buf(&c, 1)` | 栈变量 | 正常（`"p"`, `"w"`, `"d"`） |
| `write_buf("\n", 1)` | .rodata 字符串 | `\x00` |
| `write_str("cinux> ")` | .rodata 字符串 | 整个写丢失 |
| `write_str(argv[0])` | 栈 buffer | 正常（`"pwd"`, `"ls"`） |

**规律：从栈读的数据正确，从 `.rodata` 读的全是 `\x00` 或丢失。**

这说明 shell 的 `.text` 段加载正确（代码能跑），但 `.rodata` 段所在的页内容是零。

## 根因

`kernel/proc/process.cpp` execve 的 PT_LOAD 页填充循环：

```cpp
// 旧代码（有 bug）
uint64_t page_base_offset = vaddr - phdr.p_vaddr;
copy_start = page_base_offset;
// ...
inode->ops->read(inode, phdr.p_offset + copy_start, dst + copy_start, copy_len);
```

对第一页（`page_base_offset = 0`）没问题。但第二页起 `page_base_offset = 0x1000`，
而 `dst` 是一个**全新的 4KB 物理页**。写入 `dst + 0x1000` 越界到相邻内存，
`dst` 本身保持全零。

`.text` 通常在第一页（首 4KB）所以代码能执行；`.rodata` 在第二页及之后，
所以字符串常量全是 `\x00`。

## 修复

正确计算**页内偏移**（in-page offset）而非复用段内偏移：

```cpp
// 新代码
uint64_t data_vaddr  = (vaddr < phdr.p_vaddr) ? phdr.p_vaddr : vaddr;
uint64_t in_page_off = data_vaddr - vaddr;    // 页内偏移（第一页可能非零）
uint64_t seg_offset  = data_vaddr - phdr.p_vaddr;  // 段内偏移

if (seg_offset < phdr.p_filesz) {
    uint64_t copy_len = phdr.p_filesz - seg_offset;
    uint64_t avail    = PAGE_SIZE - in_page_off;
    if (copy_len > avail) copy_len = avail;

    inode->ops->read(inode, phdr.p_offset + seg_offset,
                      dst + in_page_off, copy_len);
}
```

`in_page_off`：段起始地址非页对齐时第一页有前导空白，其余页为 0。
`seg_offset`：纯粹用于计算文件读取偏移。

## 教训

1. **页对齐循环里不能把段偏移当页内偏移用**——每个页的 dst 都是从 0 开始的新 buffer。
2. **调试数据损坏问题时，区分栈和 .rodata**：栈由 kernel 显式映射并初始化，
   .rodata 由 execve 从 ELF 文件填充——加载器 bug 只影响后者。
3. **加数据内容日志**比只加字节数日志有效得多：`"\x00"` 比 `drained 1 bytes`
   立刻暴露了问题。
