# M2: Kernel Logging Enhancement

> 基于 M1 的 Ring Buffer，增强内核日志到 dmesg 级别。
> 添加日志级别过滤、时间戳、ring buffer sink、用户态读取接口。

## 目标

在现有 kprintf sink 架构上叠加 ring buffer 日志，提供结构化的内核日志系统。

## 现有代码

- `kernel/lib/kprintf.hpp` — 多 sink 注册：`kprintf_register_sink(OutputSink fn, void* ctx)`
- `kernel/lib/kprintf.cpp` — 遍历所有 sink 输出字符
- `kernel/lib/private/vkprintf_impl.hpp` — 格式化引擎
- `kernel/drivers/pit/pit.hpp` — PIT tick 计数（用作时间戳源）

## 任务清单

### T1: 日志级别定义

**文件**: `kernel/lib/klog.hpp`

```cpp
namespace cinux::lib {

enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};

// 全局过滤级别（编译期默认，运行时可调）
extern LogLevel g_log_threshold;

} // namespace cinux::lib
```

- [ ] 定义 LogLevel enum class
- [ ] 全局 `g_log_threshold` 变量（默认 INFO）
- [ ] 提供 `set_log_level(LogLevel)` 运行时调整

### T2: Kernel Log Buffer

**文件**: `kernel/lib/klog.hpp`, `kernel/lib/klog.cpp`

日志条目结构：

```cpp
struct LogEntry {
    uint64_t timestamp;    // PIT tick count
    LogLevel  level;
    char      message[LOG_MSG_MAX];  // 256 bytes
};
```

核心类：

```cpp
class KernelLog {
public:
    static KernelLog& instance();

    void log(LogLevel level, const char* fmt, ...);
    size_t read(LogEntry* buf, size_t max_entries, LogLevel min_level = LogLevel::DEBUG);

    // 统计
    size_t total_entries() const;
    size_t dropped_count() const;   // ring buffer 满时丢弃数

private:
    ConcurrentRingBuffer<LogEntry, LOG_RING_SIZE> ring_;
    size_t dropped_{0};
    uint64_t start_tick_{0};
};
```

- [ ] LogEntry 结构体定义
- [ ] KernelLog 单例，内部使用 ConcurrentRingBuffer<LogEntry, 4096>
- [ ] `log()` 方法：格式化消息 + 获取 tick + 推入 ring buffer
- [ ] `read()` 方法：批量读取（供 dmesg syscall 使用）
- [ ] dropped 计数器（ring buffer 满时递增）

### T3: kprintf Ring Buffer Sink

**文件**: `kernel/lib/klog.cpp`

- [ ] 实现 `klog_sink(char c, void* ctx)` 函数，符合 `OutputSink` 签名
- [ ] 在内核启动时调用 `kprintf_register_sink(klog_sink, &KernelLog::instance())`
- [ ] Sink 将字符追加到临时缓冲区，遇到 `\n` 时组装为 LogEntry 推入 ring buffer
- [ ] 同时支持 `klog(LogLevel, fmt, ...)` 直接 API（不走 kprintf 路径）

### T4: 便捷日志宏

**文件**: `kernel/lib/klog.hpp`

```cpp
#define klog_debug(fmt, ...) cinux::lib::KernelLog::instance().log(cinux::lib::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)  cinux::lib::KernelLog::instance().log(cinux::lib::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)  cinux::lib::KernelLog::instance().log(cinux::lib::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define klog_error(fmt, ...) cinux::lib::KernelLog::instance().log(cinux::lib::LogLevel::ERROR, fmt, ##__VA_ARGS__)
```

- [ ] 四个级别便捷宏
- [ ] 宏内部先检查 `g_log_threshold`，低于阈值直接返回（零开销）

### T5: dmesg 系统调用

**文件**: `kernel/syscall/sys_dmesg.cpp`（新增）

- [ ] 新增 syscall `sys_dmesg(char* buf, size_t len, int* out_count)`
- [ ] 从 KernelLog::instance().read() 读取条目
- [ ] 格式化为可读文本（`[LEVEL] timestamp: message\n`）
- [ ] 分配 syscall 编号（在现有 syscall 表末尾追加）
- [ ] 用户态 libc 添加 `dmesg()` wrapper

### T6: 迁移现有 kprintf 调用

**文件**: 各模块源文件

- [ ] `kernel/drivers/ahci/ahci.cpp` — `[AHCI]` 前缀改为 `klog_info`/`klog_error`
- [ ] `kernel/fs/ext2.cpp` — `[EXT2]` 前缀改为对应级别
- [ ] `kernel/mm/pmm.cpp` / `vmm.cpp` — 初始化日志改为 `klog_info`
- [ ] `kernel/proc/process.cpp` — 错误日志改为 `klog_error`
- [ ] 保留 `kprintf` 用于早期启动（ring buffer 未就绪时）

### T7: 单元测试

**文件**: `kernel/test/test_klog.cpp`

- [ ] 基本日志写入和读取
- [ ] 级别过滤生效
- [ ] ring buffer 满时 dropped 计数递增
- [ ] read() 批量读取正确
- [ ] dmesg syscall 格式化输出

## 产出物

- [ ] `kernel/lib/klog.hpp` — LogLevel + KernelLog + 便捷宏
- [ ] `kernel/lib/klog.cpp` — KernelLog 实现 + kprintf sink
- [ ] `kernel/syscall/sys_dmesg.cpp` — dmesg 系统调用
- [ ] 现有模块 kprintf 迁移
- [ ] `kernel/test/test_klog.cpp` — 单元测试
- [ ] 编译通过 + QEMU 启动 + dmesg 可读
