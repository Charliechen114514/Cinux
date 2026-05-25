# Prompt: 审查和修复教程

## 用途

让 AI 审查 `document/` 下的教程文件，确保代码准确、引用正确、风格一致、约束满足。

## 审查维度

### D1：格式检查

对照 `document/ai_prompts/writing_style.md`：
- 人称使用（"我们"而非"用户应当"）
- 情绪表达是否恰当
- 句子段落节奏（偏长句，不零碎）
- 无独立"注意点"/"关键点"标题
- 无学术论文风、API 手册风、ChatGPT 标准答案风

### D2：代码准确

对照 git diff 验证：
- 教程中的代码片段与实际源码一致
- 函数名、变量名、类名拼写正确
- 代码逻辑描述与实际实现一致
- 无不属于当前 tag 的后续代码泄露

```bash
# 获取 diff
git diff --stat {{PREV_TAG}}..{{CURRENT_TAG}} > /tmp/{{TAG}}.stat
git diff {{PREV_TAG}}..{{CURRENT_TAG}} > /tmp/{{TAG}}.diff
```

### D3：技术正确

对照 Intel SDM 验证：
- SDM 引用章节号正确，摘要准确
- OSDev Wiki URL 有效，内容摘要准确
- 所有引用必须带可验证 URL

```bash
# 读取 Intel SDM（禁止使用 MCP PDF 工具）
.venv/bin/python3 tools/read_sdm.py document/reference/intel/SDM-Vol3A-...pdf --search "关键词"
```

## 流程

1. 对指定 tag，收集 diff + 教程文件 + 研究资料
2. 按 D1 → D2 → D3 顺序审查
3. 发现问题直接修复（Edit 工具）
4. 修复后运行验证脚本：
   ```bash
   .venv/bin/python3 tools/auth_url_and_line.py \
     document/hands-on/{{TAG_NUMBER}}-*.md \
     document/read-through/{{TAG_NUMBER}}-*.md \
     document/tutorial/{{TAG_NUMBER}}-*.md \
     --max-lines 500
   ```
   退出码非零则必须修复 dead URL 或超限行数后重新验证
5. 写审查报告

## Hands-on 专项检查

- 确认无任何代码泄露
- 无代码块（除了验证命令的 bash 块）
- 无伪代码、函数签名、struct/class 定义

## Tutorial 专项检查

- 与 xv6/Linux/SerenityOS 的对比不止一段话
- 对比包含设计思路差异、数据结构选择、算法差异
