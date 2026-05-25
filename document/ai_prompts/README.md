# AI Prompts for Cinux

面向 AI 工具的操作指引。每个 prompt 定义：**读哪些文件 → 按什么流程 → 输出什么**。
本目录自包含，不依赖 `helpers/`。

## 文件清单

| 文件 | 用途 |
|------|------|
| `prompt_understand_project.md` | 让 AI 快速理解项目全貌 |
| `prompt_write_module.md` | 让 AI 写内核模块代码（含代码生成+审查+测试流程） |
| `prompt_generate_tutorial.md` | 让 AI 产出三套教程（Hands-on/Read-through/Tutorial） |
| `prompt_review_tutorial.md` | 让 AI 审查和修复已有教程 |
| `prompt_maintain_project.md` | 让 AI 日常维护（格式化、构建、检查） |
| `writing_style.md` | 教程写作风格指南（语气、人称、节奏、禁止风格） |
| `code_conventions.md` | 代码规范（命名、注释、格式化、现代 C++ 规则） |
| `templates/code_generator.md` | 代码骨架生成 prompt（空壳文件+TODO） |
| `templates/code_review.md` | 代码审查 prompt（P0-P4 五个维度） |
| `templates/test_generation.md` | 测试生成 prompt（Host + Kernel 双层） |

## 使用方式

1. 将对应 prompt 文件内容复制给 AI
2. 替换 `{{占位符}}` 为实际值
3. AI 按 prompt 中的流程执行

## 和 CLAUDE.md 的关系

`CLAUDE.md` 是 Claude Code 的主入口，定义教程工作流和调度规则。本目录是 `CLAUDE.md` 的补充——把散落的 prompt 和规范集中到一处，任何 AI 工具都能使用。

## 关键源文件索引（AI 按需读取）

- 代码规范详情 → `document/ai_prompts/code_conventions.md`
- 格式化配置 → `.clang-format`
- 教程写作风格 → `document/ai_prompts/writing_style.md`
- 路线图 → `document/todo/README.md`
- AI 主入口 → `CLAUDE.md`
- Intel SDM → `document/reference/intel/SDM-Vol*.pdf`（通过 `tools/read_sdm.py` 读取）
