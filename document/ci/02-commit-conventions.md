# Commit Conventions / 提交信息规范

Cinux 遵循 [Conventional Commits](https://www.conventionalcommits.org/) 规范。

## Format / 格式

```
type(scope): description
```

- **type**（必填）：提交类型
- **scope**（可选）：影响范围
- **description**（必填）：简短描述，不超过 72 字符

## Types / 提交类型

| type | 含义 | 是否影响代码 |
|------|------|-------------|
| `feat` | 新功能 | 是 |
| `fix` | 缺陷修复 | 是 |
| `doc` | 文档更新 | 否 |
| `style` | 代码格式（不影响逻辑） | 否 |
| `refactor` | 重构（不新增功能也不修复） | 是 |
| `perf` | 性能优化 | 是 |
| `test` | 测试补充或修正 | 是 |
| `chore` | 构建、工具、依赖维护 | 否 |
| `ci` | CI 配置变更 | 否 |

## Scopes / 范围

| scope | 说明 |
|-------|------|
| `kernel` | 内核核心逻辑 |
| `boot` | 引导加载 |
| `mm` | 内存管理 |
| `fs` | 文件系统 |
| `driver` | 设备驱动 |
| `sched` | 进程调度 |
| `user` | 用户态程序 |
| `test` | 测试框架 |
| `doc` | 文档 |
| `build` | 构建系统（CMake） |
| `tools` | 辅助工具 |

scope 为可选字段。当改动涉及多个模块时可以省略。

## Breaking Changes / 破坏性变更

在 type 后加 `!` 或在 footer 中写 `BREAKING CHANGE:`：

```
feat(mm)!: replace bitmap allocator with buddy system

BREAKING CHANGE: pmm_alloc/pmm_free API signature changed
```

## Examples / 示例

```
feat(fs): add ext2 write support
fix(mm): fix page fault in pmm_free when addr is unaligned
doc: update tutorial 028 for ext2 filesystem
style: auto-format with clang-format
refactor(kernel): extract common interrupt handler logic
perf(sched): optimize round-robin scheduling with bitmap queue
test(mm): add stress test for physical memory manager
chore: upgrade GCC toolchain requirement to 14
ci: add kernel integration test job
```

## PR Title / PR 标题规范

PR 标题应与 squash merge 后生成的 commit message 一致：

```
feat(fs): add ext2 write support
```

当一个 PR 包含多个 commit 时，PR 标题应概括整体改动，squash merge 后这些 commit 将合并为一个。

## Guidelines / 编写指南

- description 使用**祈使句**（"add feature" 而非 "added feature"）
- 不以句号结尾
- 首字母小写
- 用英文撰写，与代码风格保持一致
- 每个 commit 只做一件事——如果改动涉及多个不相关模块，拆分为多个 commit
