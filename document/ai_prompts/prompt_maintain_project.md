# Prompt: 日常维护 Cinux 项目

## 格式化检查

```bash
# 检查格式（不修改）
find kernel/ boot/ -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror

# 自动格式化
find kernel/ boot/ -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

格式化配置在 `.clang-format`，规则来源见 `document/ai_prompts/code_conventions.md`。

## 构建验证

```bash
# Release 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
```

## 测试

```bash
# Host 端单元测试
cd build && make test_host

# Kernel 端测试（mini kernel）
cd build && make run-kernel-test

# Kernel 端测试（big kernel）
cd build && make run-big-kernel-test
```

## 教程行数检查

```bash
.venv/bin/python3 scripts/check_line_limits.py document/hands-on/ document/read-through/ document/tutorial/
```

## 规范同步

如果修改了 `document/ai_prompts/code_conventions.md`，同步更新 `.clang-format`。
