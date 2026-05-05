# M1: libc 扩展到 80 Syscall

> 从当前 21 个 syscall 扩展到 80 个，覆盖 POSIX 常用接口。
> 为 musl/glibc 兼容和 CFBox 运行打基础。

## 任务清单

### T1: 文件操作补充

| # | Syscall | 状态 |
|---|---------|------|
| 5 | fstat | 缺失 |
| 6 | lstat | 缺失 |
| 7 | poll | 缺失 |
| 8 | lseek | 已有(?) |
| 10 | mprotect | F2 |
| 17 | pread64 | 缺失 |
| 18 | pwrite64 | 缺失 |
| 19 | readv | 缺失 |
| 20 | writev | 缺失 |
| 21 | access | 缺失 |
| 72 | fcntl | 缺失 |
| 73 | flock | F6 |
| 74 | fsync | 缺失 |
| 75 | fdatasync | 缺失 |
| 76 | truncate | 缺失 |
| 77 | ftruncate | 缺失 |
| 16 | ioctl | 缺失 |
| 25 | dup2 → dup3 | 缺失 |

- [ ] 每个缺失 syscall 实现 sys_xxx.cpp
- [ ] 注册到 syscall 表
- [ ] libc 添加 wrapper

### T2: 进程/信号补充

| # | Syscall | 状态 |
|---|---------|------|
| 9 | mmap | F2 |
| 11 | munmap | F2 |
| 12 | brk | F2 |
| 13 | rt_sigaction | F3 |
| 14 | rt_sigprocmask | F3 |
| 15 | rt_sigreturn | F3 |
| 56 | clone | F3 |
| 62 | kill | F3 |
| 63 | waitid | 缺失 |
| 102 | getuid | F9 |
| 104 | getgid | F9 |
| 107 | geteuid | F9 |
| 108 | getegid | F9 |
| 109 | setpgid | F3 |
| 121 | getpgid | F3 |
| 124 | getsid | F3 |
| 112 | setsid | F3 |
| 90 | chmod | F9 |
| 91 | fchmod | 缺失 |
| 92 | chown | F9 |
| 93 | fchown | 缺失 |

### T3: 时间补充

| # | Syscall | 状态 |
|---|---------|------|
| 35 | nanosleep | F5 |
| 96 | gettimeofday | 缺失 |
| 99 | clock_gettime | 缺失 |
| 100 | clock_getres | 缺失 |
| 228 | clock_nanosleep | 缺失 |

### T4: 网络 Socket

| # | Syscall | 状态 |
|---|---------|------|
| 41-55 | socket API | F7 |

### T5: IPC 补充

| # | Syscall | 状态 |
|---|---------|------|
| 29 | dup | 缺失 |
| 30 | dup2 | 缺失 |
| 31 | dup3 | 缺失 |
| 32 | pipe2 | 缺失 |
| 202 | futex | F3 |

- [ ] 逐个实现缺失 syscall
- [ ] 每个 syscall 有最小测试
- [ ] libc 完整头文件集（unistd.h, fcntl.h, sys/stat.h, errno.h 等）

## 产出物

- [ ] 约 40 个新 sys_xxx.cpp 文件
- [ ] libc 头文件 + wrapper 扩展
- [ ] syscall 数量从 21 → ~80
