# M3: TinyCC 编译器

> 移植 TinyCC 到 Cinux。在 Cinux 上自举编译 C 程序。
> 第一步实现「在 Cinux 上编译 C」的目标。

## 任务清单

### T1: TinyCC 交叉编译

- [ ] 获取 TinyCC 源码（mob branch）
- [ ] Cinux 交叉编译
- [ ] 平台适配：#ifdef __cinux__
- [ ] 需要的 libc：stdio, stdlib, string, errno, signal, wait, fork/exec

### T2: Cinux 头文件和库

- [ ] 提供 Cinux 系统头文件集合（/usr/include）
- [ ] 内核头文件导出（asm, linux compat）
- [ ] libc 静态库（供 TinyCC 链接）

### T3: 自举验证

- [ ] 在 Cinux 上运行 tcc
- [ ] 编译 hello.c → hello 可执行
- [ ] 运行编译出的 hello
- [ ] 编译更复杂的程序（文件操作、字符串处理）

## 产出物

- [ ] TinyCC 二进制
- [ ] Cinux 系统头文件
- [ ] 自举编译验证
