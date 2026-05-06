# M4: 文本编辑器 + 包管理器

> 用户态工具：简单终端编辑器 + 基础包管理器。

## 任务清单

### T1: 终端文本编辑器

**文件**: `user/editor/`（新增用户态程序）

类 nano 的终端编辑器：
- [ ] 文件打开/保存
- [ ] 光标移动（方向键）
- [ ] 文本插入/删除
- [ ] 搜索/替换
- [ ] 行号显示
- [ ] 多文件支持（buffer 切换）
- [ ] 基于 TTY 行规范原始模式

### T2: 包管理器

**文件**: `user/pkg/`（新增用户态程序）

简单包管理器：

```bash
# 安装包
cpkg install <package>

# 卸载
cpkg remove <package>

# 列表
cpkg list

# 搜索
cpkg search <keyword>
```

**包格式**：
```
package.cpkg (tar archive):
├── manifest.json    # 名称、版本、依赖、文件列表
├── files/           # 实际文件
└── scripts/
    ├── install.sh   # 安装脚本
    └── remove.sh    # 卸载脚本
```

- [ ] 包格式定义
- [ ] cpkg install：解压 + 执行安装脚本
- [ ] cpkg remove：执行卸载脚本 + 删除文件
- [ ] cpkg list：列出已安装包
- [ ] 本地仓库（/var/cache/cpkg/）
- [ ] 依赖解析（基础）

### T3: 集成测试

- [ ] 用编辑器编辑文件 + 保存
- [ ] 创建一个包 + 安装 + 验证
- [ ] 卸载 + 确认清理

## 产出物

- [ ] cedit 终端编辑器
- [ ] cpkg 包管理器
- [ ] 示例包
