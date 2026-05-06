# Branch Strategy / 分支策略

Cinux 采用 **main–develop 双主线 + 辅助分支** 模型管理代码生命周期。

## Branch Model / 分支模型

```
main ────────────── 稳定发布线（squash merge only）
│
├── develop ─────── 日常开发基线（开放 push）
│     ├── feat/xxx
│     ├── fix/xxx
│     ├── doc/xxx
│     ├── optimize/xxx
│     └── refactor/xxx
│
├── release/x.y ─── 从 develop 分出，功能冻结，仅修复
│
└── hotfix/xxx ──── 从 main 分出，紧急修复后合并回 main + develop
```

## Branch Roles / 各分支职责

### `main`

- 稳定发布线，任何时刻都应可构建、可运行
- **仅接受** squash merge PR（来自 develop 或 hotfix）
- 受保护：CI 全绿 + 至少 1 人 approve 才能合并
- 禁止直接 push

### `develop`

- 日常开发基线，所有 feature 分支从此分出
- 不设 CI 门禁，开发者可直接 push 或通过 PR 合并
- 代码不保证稳定，但应能通过编译

### Feature 分支（`feat/`、`fix/`、`doc/` 等）

- 从 `develop` 分出，开发完成后提 PR 回 `develop`
- 命名格式：`type/简短描述`，使用小写英文和连字符

| type | 用途 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat/ext2-write` |
| `fix` | 缺陷修复 | `fix/pmm-leak` |
| `doc` | 文档更新 | `doc/tutorial-028` |
| `optimize` | 性能或结构优化 | `optimize/scheduler` |
| `refactor` | 重构 | `refactor/vfs-layer` |
| `test` | 测试补充 | `test/mm-stress` |
| `chore` | 构建或工具维护 | `chore/cmake-cleanup` |

### `release/x.y`

- 从 `develop` 分出，代表即将发布的版本
- 功能冻结，仅接受 bug fix
- 测试通过后合并到 `main`（打版本 Tag）并反向合并到 `develop`

### `hotfix/xxx`

- 从 `main` 分出，用于紧急修复生产问题
- 修复后同时合并到 `main`（打版本 Tag）和 `develop`
- 命名示例：`hotfix/boot-crash`

## Branch Lifecycle / 分支生命周期

```
创建 → 开发 → 提 PR → Review → 合并 → 自动删除
```

1. 从 `develop`（或 `main`）创建分支
2. 本地开发和提交
3. 推送到远端并创建 Pull Request
4. 通过 Review 和 CI 检查
5. 维护者执行 Squash Merge
6. GitHub 自动删除已合并的源分支

## Branch Protection / 分支保护规则

| 分支 | 直推 | CI 门禁 | Review |
|------|------|---------|--------|
| `main` | 禁止 | 3 jobs 全绿 | ≥1 approve |
| `develop` | 允许 | 无 | 无 |
| 其他 | 允许 | 触发但不阻塞 | 无 |

## Prohibitions / 禁止事项

- 禁止直接 push 到 `main`
- 禁止从 `main` 直接分出 feature 分支（hotfix 除外）
- 禁止跨基线合并（如 feature 分支直接合并到 `main`）
- 禁止 force push 到任何受保护分支
- 禁止在 `release/` 分支上添加新功能

## Quick Reference / 速查

```bash
# 创建 feature 分支
git checkout develop
git pull
git checkout -b feat/my-feature

# 开发完成后提 PR
git push -u origin feat/my-feature
# → 在 GitHub 上创建 PR: feat/my-feature → develop

# develop → main 发版
# → 在 GitHub 上创建 PR: develop → main（squash merge）
```
