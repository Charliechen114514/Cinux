# Tag & Release / 标签与发版

Cinux 采用**双 Tag 体系**：教学里程碑 Tag 和语义化版本 Tag 并存。

## Dual Tag System / 双 Tag 体系

| Tag 类型 | 格式 | 用途 | 打 Tag 时机 |
|----------|------|------|------------|
| 教学里程碑 | `NNN_descriptive_name` | 标记教学进度节点 | milestone 完成后 |
| 版本发布 | `vX.Y.Z` | 标记可发布版本 | release/hotfix 合并到 main 后 |

### 教学里程碑 Tag

沿用现有的三位数编号体系，与 OS 开发教学进度对齐。

```
000_env_toolchain
001_boot_real_mode
002_boot_gdt_protected
...
028_fs_ext2
035_multi_terminal
```

- 编号为三位数字，零填充
- 描述使用 snake_case
- 在 develop 或 main 上打 Tag，取决于 milestone 完成时所在的分支

### 版本发布 Tag

遵循 [Semantic Versioning](https://semver.org/) 规范。

```
v0.1.0    ← 初始可启动版本
v0.2.0    ← 加入内存管理
v0.3.0    ← 加入文件系统
```

- `X`（主版本号）：不兼容的 API 变更
- `Y`（次版本号）：向后兼容的功能新增
- `Z`（修订号）：向后兼容的缺陷修复
- **仅在 main 分支上打版本 Tag**

## Release Branch Workflow / 发布分支流程

```
develop ──→ release/x.y ──→ main（打 v Tag）
              │
              └──→ develop（反向合并修复）
```

### 详细步骤

1. **创建 release 分支**：当 develop 积累的功能足够发版时
   ```bash
   git checkout develop
   git pull
   git checkout -b release/0.3
   git push -u origin release/0.3
   ```

2. **功能冻结**：release 分支上仅接受 bug fix，不添加新功能

3. **测试与修复**：在 release 分支上进行集成测试，修复发现的问题
   ```bash
   git checkout release/0.3
   # 修复 bug...
   git commit -m "fix(mm): fix page fault under high memory pressure"
   git push
   ```

4. **合并到 main**：测试通过后提 PR
   ```
   base: main ← compare: release/0.3
   ```
   CI 全绿 + approve 后 squash merge

5. **打版本 Tag**：
   ```bash
   git checkout main
   git pull
   git tag -a v0.3.0 -m "Release v0.3.0: ext2 write + scheduler optimization"
   git push origin v0.3.0
   ```

6. **反向合并到 develop**：将 release 分支上的修复带回 develop
   ```bash
   git checkout develop
   git merge release/0.3
   git push
   ```

7. **删除 release 分支**

## Hotfix Workflow / 紧急修复流程

```
main ──→ hotfix/xxx ──→ main（打 v Tag，Z+1）
              │
              └──→ develop
```

### 详细步骤

1. 从 main 创建 hotfix 分支并修复
2. 提 PR 到 main，CI 全绿 + approve 后 squash merge
3. 打补丁版本 Tag：
   ```bash
   git tag -a v0.3.1 -m "Hotfix: boot crash on BIOS without A20 support"
   git push origin v0.3.1
   ```
4. 合并修复到 develop
5. 删除 hotfix 分支

## Version Numbering Guidelines / 版本号指南

作为教学 OS 项目，主版本号在 1.0 之前遵循以下惯例：

- `v0.Y.Z`：项目处于活跃开发期，API 可能频繁变更
- `Y` 递增：新增教学里程碑或重大功能模块
- `Z` 递增：bug fix 和小改进

建议里程碑与版本的对应关系在 `README.md` 中维护。

## GitHub Release / GitHub 发布

打完版本 Tag 后，在 GitHub 上创建 Release：

1. 进入仓库 → Releases → Draft a new release
2. 选择版本 Tag（如 `v0.3.0`）
3. 填写 Release title 和 Release notes
4. Release notes 建议包含：
   - 本版本新增的教学里程碑
   - 新功能和改进列表
   - 已知问题
5. 发布

## Tag Commands Quick Reference / Tag 命令速查

```bash
# 打轻量 Tag
git tag 028_fs_ext2

# 打附注 Tag（推荐用于版本 Tag）
git tag -a v0.3.0 -m "Release v0.3.0"

# 推送 Tag
git push origin v0.3.0

# 推送所有 Tag
git push origin --tags

# 删除远端 Tag
git push origin --delete v0.3.0

# 列出所有 Tag
git tag -l

# 查看 Tag 信息
git show v0.3.0
```
