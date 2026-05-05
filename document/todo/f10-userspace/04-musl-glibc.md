# M5: musl libc 接入 + glibc 兼容验证

> 三阶段 libc 路线：自建(当前) → musl(本 milestone) → glibc 兼容(目标验证)。
> musl 作为过渡，验证内核 POSIX 兼容性。

## 目标

1. musl libc 交叉编译到 Cinux 并验证基本程序运行
2. glibc 静态二进制兼容性验证（目标明确化）

## 任务清单

### T1: musl libc 交叉编译

- [ ] 获取 musl libc 源码
- [ ] 配置交叉编译工具链（Cinux GCC 14 target）
- [ ] 实现 musl 需要的内核接口（系统调用 wrapper）
- [ ] 编译 musl 动态库 + crt 启动文件
- [ ] 安装到 sysroot：/lib/ld-musl-x86_64.so.1

### T2: musl 兼容性验证

逐步验证 musl 编译的程序：

**基础验证**：
- [ ] hello world（printf）
- [ ] 文件操作（fopen/fread/fwrite）
- [ ] malloc/free
- [ ] 字符串操作

**进程验证**：
- [ ] fork + execve
- [ ] waitpid
- [ ] pipe + dup2
- [ ] signal (SIGINT/SIGTERM)

**网络验证**（依赖 F7）：
- [ ] socket + connect
- [ ] DNS 解析（getaddrinfo）

**CFBox 验证**：
- [ ] CFBox 使用 musl 编译
- [ ] 基本命令运行（ls, cat, cp, sh）
- [ ] pipe + 重定向

### T3: musl 集成到构建系统

- [ ] CMake toolchain file 指定 musl sysroot
- [ ] 用户程序默认使用 musl 编译
- [ ] 保留自建 libc 作为内核测试用

### T4: glibc 静态二进制兼容验证

明确 glibc 兼容的验证目标：

| 验证项 | 说明 |
|--------|------|
| 静态编译 hello world | glibc -static 编译 → Cinux 直接运行 |
| BusyBox 静态 | busybox -static → Cinux 运行 |
| 动态链接 | 需 F10 M2 ELF 动态链接器完善 |
| 预编译二进制 | 目标：能跑 Ubuntu apt 下载的预编译二进制 |

- [ ] 在 Linux 上用 glibc -static 编译测试程序
- [ ] 在 Cinux 上运行静态二进制
- [ ] 记录不兼容的 syscall 并修复
- [ ] 目标里程碑：BusyBox 静态版在 Cinux 运行

### T5: 缺失 syscall 补齐

根据 musl/glibc 测试结果补齐：

| 类别 | 可能缺失的 syscall |
|------|-------------------|
| 进程 | waitid, execveat |
| 文件 | statx, utimensat, faccessat, unlinkat, mkdirat, readlinkat |
| 内存 | mremap, madvise |
| 时间 | clock_nanosleep, timer_create, timer_settime |
| 信号 | rt_sigpending, rt_sigsuspend, sigaltstack |
| IPC | eventfd2, signalfd4, timerfd_create/settime |
| 网络 | accept4, socketpair, recvmmsg, sendmmsg |

- [ ] 按测试结果逐个补齐
- [ ] 每个新 syscall 有基本测试

## 产出物

- [ ] musl libc 交叉编译工具链
- [ ] musl 编译的程序在 Cinux 运行
- [ ] glibc 静态二进制验证报告
- [ ] 缺失 syscall 补齐
- [ ] CMake toolchain 文件
