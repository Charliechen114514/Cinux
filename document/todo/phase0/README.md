# Phase 0: 基础加固（严格串行）

> 所有 F1-F13 的前置。完成后才开始 Feature 域开发。

## 核心决策摘要

| 决策 | 结论 |
|------|------|
| 执行方式 | 严格串行：0-A → 0-B → 0-C |
| C++23 标准 | 激进采用（concepts, expected, constexpr） |
| 接口变更 | 可改签名但必须同步更新所有调用方 |
| 测试验收 | 现有测试通过 + 补充缺失测试 |
| 模块优化顺序 | 低依赖优先（lib → mm → fs → drivers → proc → syscall → ipc → gui → arch） |
| 注释语言 | 英文 |
| 注释规范 | 全套 CFDesktop Doxygen 规范 |
| 代码风格 | BSD/Allman（保持现状） |
| 错误处理 | 内核内部 ErrorOr<T>（Expected 模式） |

## 文件清单

| 文件 | 子阶段 | 说明 |
|------|--------|------|
| [00-cmake.md](00-cmake.md) | 0-A | CMake 架构升级 + 大文件拆分 |
| [01-code-optimization.md](01-code-optimization.md) | 0-B | 9 模块 C++23 现代化 |
| [02-comments.md](02-comments.md) | 0-C | Doxygen 注释全量覆盖 |

## 验收标准

每个子阶段：
1. `cmake --build build/` 编译通过
2. 现有测试全部通过
3. 每文件 ≤ 500 行（0-A 建立硬上限）
