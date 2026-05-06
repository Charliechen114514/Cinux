# CI Pipeline / 持续集成流水线

Cinux 使用 GitHub Actions 作为 CI 平台，配置文件位于 `.github/workflows/ci.yml`。

## Pipeline Overview / 流水线概览

```
┌─────────────────────────────────────────┐
│           CI Pipeline                   │
│                                         │
│  ┌─────────┐                            │
│  │ format  │ ← clang-format 自动格式化   │
│  └────┬────┘                            │
│       │                                 │
│  ┌────┴──────────┐                      │
│  │  host-tests   │ ← 主机端单元测试      │
│  └───────────────┘                      │
│                                         │
│  ┌───────────────┐                      │
│  │ kernel-tests  │ ← QEMU 内核集成测试   │
│  └───────────────┘                      │
└─────────────────────────────────────────┘
```

- `host-tests` 和 `kernel-tests` 并行执行（均依赖 `format` 先通过）
- `format` 会自动提交格式化修改到当前分支

## Trigger Rules / 触发规则

CI 在以下事件触发：

| 事件 | 触发条件 |
|------|---------|
| Pull Request | 目标分支为 `main` 或 `develop` |
| Push | 推送到 `main` 分支 |

对应的配置：

```yaml
on:
  push:
    branches: [main]
  pull_request:
    branches: [main, develop]
```

## Jobs / 任务说明

### Format Check / 代码格式检查

| 项目 | 说明 |
|------|------|
| 运行环境 | `ubuntu-latest` |
| 超时 | 5 分钟 |
| 工具 | `clang-format` |
| 扫描范围 | `kernel/`、`user/`、`test/`、`boot/` 下的 `.cpp` `.hpp` `.c` `.h` 文件 |
| 行为 | 自动格式化并提交修改（commit message: `style: auto-format with clang-format`） |

### Host Tests / 主机端测试

| 项目 | 说明 |
|------|------|
| 运行环境 | `ubuntu-latest` |
| 超时 | 15 分钟 |
| 依赖 | GCC 14, CMake (pip), QEMU, e2fsprogs, Python 3 |
| 构建配置 | `cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON` |
| 测试命令 | `make test_host` |

此 job 在主机端编译并运行单元测试，验证内核各模块（内存管理、文件系统等）的逻辑正确性。

### Kernel Tests / 内核集成测试

| 项目 | 说明 |
|------|------|
| 运行环境 | `ubuntu-latest` |
| 超时 | 15 分钟 |
| 依赖 | 同 Host Tests |
| 构建配置 | 同 Host Tests |
| 测试命令 | `make run-kernel-test` |

此 job 将内核镜像在 QEMU 中启动并运行集成测试，验证完整的内核启动和运行流程。

## CI Environment / CI 环境

| 组件 | 版本 / 来源 |
|------|------------|
| OS | Ubuntu Latest（GitHub Actions 标准镜像） |
| GCC | 14（通过 `apt-get install gcc-14 g++-14`） |
| CMake | 最新版（通过 `pip install cmake`） |
| QEMU | `qemu-system-x86`（通过 `apt-get`） |
| Python | 3（系统自带） |

## Modifying CI / 修改 CI 配置

CI 配置文件：`.github/workflows/ci.yml`

修改 CI 的典型场景：

1. **添加新触发分支**：在 `on:` 节点中修改 `branches` 列表
2. **添加新 Job**：在 `jobs:` 节点中添加，注意 `needs` 依赖关系
3. **调整超时**：修改 `timeout-minutes` 值
4. **更新工具版本**：修改 Install 步骤中的版本号

修改 CI 配置的 commit 应使用 `ci:` 前缀：
```
ci: add code coverage report job
```

## Future Enhancements / 未来扩展方向

以下功能可在项目成熟后逐步引入：

- **代码覆盖率**：添加 `gcov` / `lcov` 步骤，生成覆盖率报告
- **多工具链矩阵**：同时测试 GCC 14 和 Clang 18
- **Release Artifact**：在 main 分支构建产物并上传为 Release Asset
- **定时回归测试**：使用 `schedule` 触发器每日运行完整测试
- **依赖缓存**：缓存 CMake 构建产物以加速 CI
