# M2: ProcFS

> /proc 虚拟文件系统。暴露内核状态给用户态。
> 进程信息、内存统计、内核参数。

## 目标

实现 ProcFS，提供系统自省能力。类似 Linux /proc。

## 任务清单

### T1: ProcFS 框架

**文件**: `kernel/fs/procfs.hpp`, `kernel/fs/procfs.cpp`

```cpp
class ProcFS : public FileSystem {
public:
    bool mount() override;
    Inode* lookup(const char* path) override;

private:
    // /proc 下的静态文件
    struct ProcEntry {
        const char* name;
        Inode inode;
        int (*read_func)(char* buf, size_t len);  // 内容生成函数
    };

    ProcEntry entries_[PROC_MAX_ENTRIES];
    int entry_count_;
};
```

- [ ] ProcFS 继承 FileSystem
- [ ] mount() 注册标准条目
- [ ] lookup() 按 name 查找
- [ ] read() 调用对应生成函数

### T2: 标准条目

| 路径 | 内容 |
|------|------|
| /proc/version | 内核版本字符串 |
| /proc/meminfo | 内存统计（total/free/cached） |
| /proc/cpuinfo | CPU 信息（vendor/model/freq） |
| /proc/uptime | 系统运行时间（秒） |
| /proc/loadavg | 负载平均值 |
| /proc/cmdline | 启动参数 |

- [ ] 每个条目一个生成函数
- [ ] 格式参考 Linux /proc 对应文件

### T3: 进程条目

| 路径 | 内容 |
|------|------|
| /proc/[pid]/ | 进程目录 |
| /proc/[pid]/status | 进程状态（PID/PPID/state/name） |
| /proc/[pid]/maps | 内存映射（VMA 列表） |
| /proc/[pid]/fd/ | 打开文件列表 |
| /proc/[pid]/stat | 简洁进程状态（一行） |
| /proc/[pid]/cmdline | 进程命令行 |

- [ ] 数字目录名解析为 PID
- [ ] 从 Task 结构体提取信息
- [ ] maps 列出所有 VMA 区域

### T4: 单元测试

- [ ] mount /proc 成功
- [ ] 读取 /proc/version 有内容
- [ ] /proc/1/status 显示 init 进程
- [ ] ls /proc 显示所有条目

## 产出物

- [ ] `kernel/fs/procfs.hpp` / `.cpp`
- [ ] 标准 + 进程条目
- [ ] mount /proc 集成到启动流程
