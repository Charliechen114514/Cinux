# Prompt: 编写内核模块代码

## 用途

让 AI 完成一个里程碑的代码开发：从需求分析到代码实现、测试、审查。

## 项目约束

- 工具链：GNU AS（AT&T 语法）+ GCC/G++ + CMake
- C++ 规范：`-std=c++23 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -mno-red-zone -mcmodel=kernel`
- 禁用标准库，允许模板/constexpr/concepts/RAII/lambda
- 注释语言：**英文**（Doxygen、行内注释、TODO、汇编注释全部英文）
- 代码写在 `kernel/` 目录下（大内核），新文件需更新 `kernel/CMakeLists.txt`
- 内核链接脚本：`kernel/linker.ld`（`KERNEL_VMA = 0xFFFFFFFF80000000`，加载地址 `0x1000000`）

## 流程

### Step 1：获取里程碑信息

1. 读 `document/todo/README.md`，找到当前待完成的里程碑
2. 执行 `git tag --sort=-creatordate | head -5`，获取 current_tag、prev_tag
3. 提取：里程碑名称、目标效果、checklist 清单

### Step 2：编写代码

1. 读 `document/ai_prompts/templates/code_generator.md` 获取代码生成规范
2. 读 `document/ai_prompts/code_conventions.md` 了解代码风格
3. 读同目录已有代码，理解现有模式
4. 按 checklist 逐项编写代码，写入对应源文件
5. 代码必须遵守 `code_conventions.md` 中的所有规则

**现代 C++ 硬性要求**：
- 硬件状态封装在 class 中，不用裸全局变量 + 包裹函数
- `enum class` 作为 API 类型，不立刻 cast 到整数
- 常量用 `constexpr`，不用 `#define`（include guard 除外）
- 能 `static_assert` 检查的都加上
- 用 `using` 不用 `typedef`
- 用 `static_cast<>/reinterpret_cast<>` 不用 C 风格转型
- `.hpp` 只放声明，`.cpp` 放实现
- `extern "C"` 使用块语法 `{ ... }`

### Step 3：编写测试

1. 读 `document/ai_prompts/templates/test_generation.md` 获取测试规范
2. 按模板**依次**完成：
   - **Phase A**：Host 端单元测试（`test/unit/test_{{module}}.cpp`）
     - 设计 mock 隔离硬件依赖
     - 覆盖正常路径、边界条件、错误处理
     - 在 `test/CMakeLists.txt` 中注册测试目标
   - **Phase B**：Kernel 端测试（`kernel/test/test_{{module}}.cpp`）
     - 直接调用内核模块接口，无需 mock
     - 在 `kernel/CMakeLists.txt` 中注册（`big_kernel_test` 目标）
     - 在 `kernel/test/main_test.cpp` 中添加入口调用
3. Host 和 Kernel 测试都必须生成

### Step 4：构建 + 测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build && make test_host          # Host 端测试，必须通过
cd build && make run-kernel-test    # Kernel 端测试，必须通过
```

- 构建失败：分析报错，修复代码，重新构建（最多 3 轮）
- 测试失败：分析原因，修复代码，重新构建+测试（最多 3 轮）
- 超过 3 轮仍失败，暂停请用户介入

### Step 5：代码审查（可选）

1. 读 `document/ai_prompts/templates/code_review.md` 了解审查规范
2. 对本次变更进行审查，返回改进建议（不改代码）
3. 等用户决定是否采纳建议

## 异常处理

| 情况 | 处理 |
|------|------|
| 构建失败 | 自动修复，最多 3 轮后暂停请用户介入 |
| 测试失败 | 同上 |
| 想跳过某步骤 | 用户说「跳过 review」或「跳过测试」即可 |
| 中途退出 | 下次从指定步骤继续 |

## 快速参考

```
开始 milestone {{tag}}
继续 milestone {{tag}}，从步骤 {{N}} 开始
```
