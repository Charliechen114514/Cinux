# Pull Request Workflow / PR 工作流

## Overview / 概述

Cinux 的 PR 流程按目标分支分为四种场景，每种场景的门禁要求不同。

| PR 方向 | CI 门禁 | Review | 合并方式 |
|---------|---------|--------|---------|
| feature → develop | 触发但不阻塞 | 无 | merge commit 或 squash |
| develop → main | **必须全绿** | ≥1 approve | **squash merge** |
| release/x.y → main | **必须全绿** | ≥1 approve | **squash merge** |
| hotfix → main + develop | **必须全绿**（main 方向） | ≥1 approve（main 方向） | **squash merge** |

## Feature → Develop / 功能开发合并

最常见的 PR 类型。feature 分支完成开发后提 PR 到 develop。

### 步骤

1. 确保分支从最新 develop 创建
2. 本地开发和测试
3. 推送到远端并创建 PR：
   ```
   base: develop ← compare: feat/my-feature
   ```
4. CI 会触发但**不阻塞合并**，开发者可自行决定是否等待 CI 通过
5. 自行合并或请维护者合并

### 建议

- 推送前本地运行 `cmake --build build` 确保编译通过
- 大改动建议先开 issue 讨论

## Develop → Main / 发版合并

develop 积累了足够的功能后，提 PR 合并到 main 进行发版。

### 步骤

1. 确认 develop 上的功能已经过充分测试
2. 创建 PR：
   ```
   base: main ← compare: develop
   ```
3. 等待 CI 全绿（format + host tests + kernel tests）
4. 至少 1 位维护者 approve
5. 维护者执行 **Squash Merge**，PR 标题将成为 main 上的 commit message
6. 合并后打版本 Tag（参见 `04-tag-and-release.md`）

### PR 标题格式

```
feat: release v0.3.0 — ext2 write + scheduler optimization
```

## Release → Main / 发布分支合并

release 分支测试完毕后合并到 main。

### 步骤

1. release 分支上的 bug fix 已完成
2. 创建 PR：
   ```
   base: main ← compare: release/0.3
   ```
3. CI 全绿 + approve 后 squash merge
4. 合并后打版本 Tag
5. 将 release 分支的修复合并回 develop

## Hotfix → Main + Develop / 紧急修复

紧急修复需要同时应用到 main 和 develop。

### 步骤

1. 从 main 创建 hotfix 分支：
   ```bash
   git checkout main
   git pull
   git checkout -b hotfix/boot-crash
   ```
2. 修复并测试
3. 创建 PR 到 main：
   ```
   base: main ← compare: hotfix/boot-crash
   ```
4. CI 全绿 + approve 后 squash merge，打补丁版本 Tag
5. 将修复合并到 develop：
   ```bash
   git checkout develop
   git merge hotfix/boot-crash
   git push
   ```
6. 删除 hotfix 分支

## PR Template / PR 模板

建议在 PR 描述中包含以下内容：

```markdown
## Summary / 概述
<!-- 一两句话描述改动 -->

## Related Tag / Issue
<!-- 关联的教学里程碑 Tag 或 Issue 编号 -->

## Checklist / 检查清单
- [ ] 本地编译通过
- [ ] 本地测试通过（host tests + kernel tests）
- [ ] Commit message 遵循 Conventional Commits
- [ ] 无无关改动
```

## Squash Merge Operation / Squash Merge 操作

在 GitHub PR 页面：

1. 点击 **Merge pull request** 按钮旁的下拉箭头
2. 选择 **Squash and merge**
3. 确认标题遵循 `type(scope): description` 格式
4. 点击 **Confirm squash and merge**

合并后 GitHub 会自动删除源分支（需在仓库设置中启用）。

## Branch Auto-Delete / 分支自动删除配置

在 GitHub 仓库 Settings → General → Pull Requests 中勾选：

> ☑ Automatically delete head branches

这确保 PR 合并后源分支自动清理，保持仓库整洁。
