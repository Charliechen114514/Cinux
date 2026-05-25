# Prompt: 生成教程

## 用途

让 AI 为指定 git tag 生成三套教程：Hands-on（动手版）、Read-through（通读版）、Tutorial（发布版）。

## 三类教程定义

| Type | 定位 | 代码策略 | 输出目录 |
|------|------|----------|----------|
| Hands-on | OS 作业式引导 | 纯设计描述+验证，**绝不展示代码** | `document/hands-on/` |
| Read-through | 完整代码讲解 | 完整代码 walkthrough | `document/read-through/` |
| Tutorial | 最终发布版（替代 blog） | 叙事体+代码精讲+引用 | `document/tutorial/` |

## 硬规则（违反任何一条即需重写）

1. **最低 3 篇**：每种教程类型至少拆分为 3 篇文章
2. **单篇篇幅上限 500 行**：每篇控制在 200–500 行，超过 500 行立即拆分
3. **所有引用必须带可验证 URL** 或 Intel SDM 页码章节
4. **Hands-on 绝不展示代码**：不展示代码块、伪代码、函数签名、struct/class 定义
5. **严格遵循写作风格**：读 `document/ai_prompts/writing_style.md`

## 拆分维度参考

- 按子功能拆分（初始化 vs 运行时、数据结构 vs 算法、硬件交互 vs 软件逻辑）
- 按源文件拆分（一个源文件或一对 hpp/cpp 对应一篇）
- 按测试/验证拆分（核心实现单列一篇，测试策略和结果单列一篇）
- 按概念深度拆分（基础概念一篇、实现细节一篇、扩展对比一篇）
- 按调用层级拆分（底层辅助函数一篇、核心 API 一篇、上层集成一篇）

## Pipeline（5 Phase，严格串行）

```
Tag N ──► Phase 1: Research ──────► Phase 2: Hands-on ──────► Phase 3: Read-through
          │                        │                          │
          │ Phase 4: Tutorial ◄────┘                          │
          │                        ◄──────────────────────────┘
          └─► Phase 5: Review ────► DONE
```

**除了 Phase 1 需要确认拆分方案，其他不需要用户确认，直接推进。**

---

## Phase 1: Research（研究 + 拆分决策）

### 工作内容

1. **Git Diff 分析**：
   ```bash
   git diff --stat {{PREV_TAG}}..{{CURRENT_TAG}}
   git diff {{PREV_TAG}}..{{CURRENT_TAG}}
   ```
   提取：变更文件列表、新增/修改/删除行数、涉及的子系统、引入的核心概念

2. **资料检索**（按优先级）：

   **2a: Intel SDM**（最高优先级）
   - 必须且只能用 `tools/read_sdm.py`（通过 Bash 调用），禁止 MCP PDF 工具
   ```bash
   .venv/bin/python3 tools/read_sdm.py document/reference/intel/SDM-Vol3A-...pdf --search "A20 Gate"
   .venv/bin/python3 tools/read_sdm.py document/reference/intel/SDM-Vol3A-...pdf --pages 100 120
   .venv/bin/python3 tools/read_sdm.py document/reference/intel/SDM-Vol3A-...pdf --chapter "Protection"
   ```
   - 必须标注：卷号 + 章节标题 + 页码范围（如 "Vol.3A §9.1 p.9-3~9-5"）

   **2b: OSDev Wiki**
   - 搜索 + 读取页面
   - 必须包含页面 URL

   **2c: 其他 OS 实现**
   - 经典教学 OS：xv6, MIT JOS, PintOS
   - 生产级 OS：Linux 早期代码, SerenityOS, ToaruOS
   - 必须包含 GitHub 仓库或源文件 URL

3. **Notes 提取**：读 `document/notes/{{TAG_NUMBER}}/` 下所有文件

4. **拆分决策**：基于 diff 复杂度和概念数量，决定每种教程类型的拆分方案
   - 每种至少 3 篇，每篇预估 200-300 行
   - 标注每篇标题和预估行数

### 输出
写入 research brief 文件（路径根据项目实际结构决定）

---

## Phase 2: Hands-on（动手版，绝不展示代码）

### 约束
- **绝不展示最终代码** — 无代码块、无伪代码、无函数签名、无 struct/class 定义
- 只描述：目标、设计思路、接口约束（文字描述）、硬件行为、验证命令
- 每步结尾给**验证命令**（构建命令 + 预期输出描述）
- 包含踩坑预警

### 文章结构
```
# {{PHASE_TITLE}}

## 导语（~150字）
- 本章做什么、为什么必要、完成后效果
- 知识前置要求

## 概念精讲
- 通俗语言解释
- 类比或图示（ASCII art / Mermaid）
- 相关硬件行为

## 动手实现
### Step 1: [操作名称]
**目标**: 这一步要达成什么
**设计思路**: 为什么这样做，底层机制
**实现约束**: 纯文字描述接口和数据结构
**踩坑预警**: 常见错误和排查方法
**验证**: 运行什么命令，看到什么输出

### Step 2: ...

## 构建与运行
## 调试技巧
## 本章小结
```

### 输出
写入 `document/hands-on/{{TAG_NUMBER}}-{{tag-name}}-{SEQ}.md`

---

## Phase 3: Read-through（通读版，完整代码讲解）

### 约束
- 包含完整代码，按逻辑功能段拆分讲解
- 代码块后紧跟正文讲解（为什么这样写、有什么需要注意的）
- **禁止**独立起"注意点"/"关键点"标题 — 踩坑提醒融进行文
- 包含架构图/数据流图（Mermaid/ASCII）
- 重复模式的测试代码按组归类讲解，不逐个粘贴

### 文章结构
```
# {{ARTICLE_TITLE}}

## 概览（~200字）
## 架构图
## 代码精讲
### [功能段标题]
代码块（按功能段拆分）→ 正文讲解

## 设计决策
### 决策：[标题]
**问题**: 面临什么选择
**本项目的做法**: ...
**备选方案**: （至少一个）
**为什么不选备选方案**: ...
**如果要扩展/改进**: ...

## 扩展方向
## 参考资料
```

### 输出
写入 `document/read-through/{{TAG_NUMBER}}-{{tag-name}}-{SEQ}.md`

---

## Phase 4: Tutorial（发布版，叙事体+精讲）

### 约束
- **叙事体 + 代码精讲融合**：不是代码生成机，是在教人
- **设计思路对比**（与 xv6/Linux/SerenityOS 等，不止一段话）：
  - 对比维度：设计思路差异、数据结构选择、算法差异
  - 分析为什么各项目做了不同选择
- **Intel SDM 精确引用**（卷号.章节.小节 + 关键段落摘要）
- 必须解释"为什么"

### 文章结构
```
# {{ARTICLE_TITLE}}

## 前言/动机段（必须存在）
## 环境说明
## 分阶段推进
### [行动导向标题]
- 目标 → 为什么 → 代码段 → 讲解
- 踩坑预警融入叙述
- 设计对比（不止一段话）

## 收尾
## 参考资料
```

### 输出
写入 `document/tutorial/{{TAG_NUMBER}}-{{tag-name}}-{SEQ}.md`

---

## Phase 5: Review（审查）

### 检查维度
1. **代码准确性**：与 git diff 一致，函数名/变量名/类名正确
2. **引用准确性**：SDM 章节号正确、OSDev URL 有效
3. **风格一致性**：对照 `document/ai_prompts/writing_style.md`
4. **跨教程一致性**：三套教程间同一概念描述一致、术语统一
5. **Hands-on 约束**：确认无代码泄露
6. **Tutorial 对比充分性**：与 xv6/Linux 等对比不止一段话

### 修复后验证
```bash
.venv/bin/python3 tools/auth_url_and_line.py \
  document/hands-on/{{TAG_NUMBER}}-*.md \
  document/read-through/{{TAG_NUMBER}}-*.md \
  document/tutorial/{{TAG_NUMBER}}-*.md \
  --max-lines 500
```
退出码非零则必须修复后重新验证。

### 输出
写审查报告。

---

## 资料检索优先级

1. **Intel SDM** → 必须用 `tools/read_sdm.py`，禁止 MCP PDF 工具
2. **OSDev Wiki** → 搜索 + 读取
3. **网络搜索不可用** → 暂停，请用户提供外部研究结果

## 高频叙事连接句（必须模仿）

```
我们现在要做的是
先别急
这里先验证一下
你会发现
很好，现在
接下来问题来了
我们回头看
事情到这里还没完
真正的坑在后面
```

## 输出命名

`{TAG_NUMBER}-{tag-name}-{SEQ}.md`
- TAG_NUMBER: `000`, `028b` 等
- tag-name: 小写+连字符
- SEQ: 从 1 开始
