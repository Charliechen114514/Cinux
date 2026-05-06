# M2: ELF 动态链接器

> 支持 ELF 动态段、PLT/GOT 重定位、共享库加载。
> 运行动态链接程序的基础。

## 目标

实现内核侧 ELF 动态加载支持 + 用户态 ld.so 等价物。

## 任务清单

### T1: PT_INTERP 段处理

**文件**: `kernel/proc/process.cpp`

- [ ] execve 检测 PT_INTERP 段（指定动态链接器路径，如 /lib/ld-cinux.so.1）
- [ ] 加载动态链接器 ELF 到地址空间
- [ ] 设置入口为 ld.so 的 e_entry（而非主程序）
- [ ] 传递辅助向量（auxv）给 ld.so：AT_ENTRY, AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_BASE

### T2: PT_DYNAMIC 段处理

- [ ] 解析 .dynamic 段
- [ ] DT_NEEDED：列出依赖的共享库
- [ ] DT_RPATH/DT_RUNPATH：库搜索路径

### T3: 全局偏移表 (GOT) + PLT

- [ ] GOT 重定位：R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
- [ ] PLT 延迟绑定（lazy binding）
- [ ] 符号查找：在主程序和所有共享库中搜索

### T4: 内核侧 ELF 扩展

**文件**: `kernel/proc/elf_types.hpp`

- [ ] 新增 Elf64_Dyn 结构体
- [ ] 新增 Elf64_Rela 重定位条目
- [ ] 新增 Elf64_Sym 符号表条目
- [ ] PT_INTERP / PT_DYNAMIC 解析函数

### T5: 用户态动态链接器

**文件**: `user/ld-cinux/ld_cinux.c`（新增用户态程序）

最小动态链接器：
- [ ] 解析辅助向量获取主程序信息
- [ ] 加载 DT_NEEDED 共享库
- [ ] 执行重定位
- [ ] 初始化 .init_array
- [ ] 跳转到主程序 entry

## 产出物

- [ ] 内核 ELF 动态段支持
- [ ] 辅助向量传递
- [ ] 用户态 ld-cinux 动态链接器
- [ ] 运行动态链接的 hello world 测试
