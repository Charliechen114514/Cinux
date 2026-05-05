# M2: Lua 5.4 脚本

> 移植 Lua 5.4 到 Cinux。系统脚本和自动化。
> 用户态程序，通过 libc syscall 访问内核功能。

## 任务清单

### T1: Lua 交叉编译

- [ ] 获取 Lua 5.4 源码
- [ ] Cinux 交叉编译工具链编译
- [ ] 实现 luaconf.h 平台适配（Cinux libc 兼容）
- [ ] 需要的 libc 函数：stdio, stdlib, string, math, time

### T2: Cinux Lua 扩展模块

**文件**: `user/lua/cinux_module.c`

暴露 Cinux 特有功能给 Lua：
```lua
-- 系统信息
cinux.version()
cinux.uptime()
cinux.meminfo()

-- 进程管理
cinux.fork()
cinux.exec(path)
cinux.kill(pid, sig)

-- 文件操作（使用 Lua io 库即可）
-- 网络操作（如果有 F7）
```

- [ ] cinux 模块实现
- [ ] 注册到 Lua 注册表

### T3: 集成测试

- [ ] Lua 解释器在 Cinux 运行
- [ ] 执行 hello.lua
- [ ] 文件读写测试
- [ ] cinux 模块功能测试

## 产出物

- [ ] Lua 5.4 二进制（Cinux 可执行格式）
- [ ] cinux Lua 模块
- [ ] 示例脚本
