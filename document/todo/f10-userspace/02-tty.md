# M3: TTY 子系统

> 终端子系统：行规范(line discipline)、伪终端(PTY)。
> Shell 管道、Job Control、交互式终端的基础。

## 目标

1. 行规范：行编辑、回显、Ctrl+C 信号生成
2. 伪终端(PTY)：pty master/slave 对，用于终端模拟器

## 任务清单

### T1: TTY 核心抽象

**文件**: `kernel/drivers/tty/tty.hpp`, `kernel/drivers/tty/tty.cpp`

```cpp
class TTY {
public:
    // 输入路径：键盘 → TTY → 进程
    void input_char(char c);     // 键盘输入一个字符

    // 输出路径：进程 → TTY → console
    int64_t write(const char* buf, size_t len);

    // 行规范读取（进程 read）
    int64_t read(char* buf, size_t len);

    // 配置
    struct Termios {
        uint32_t c_iflag;  // 输入模式
        uint32_t c_oflag;  // 输出模式
        uint32_t c_cflag;  // 控制模式
        uint32_t c_lflag;  // 本地模式 (ECHO, ICANON, ISIG)
        uint8_t  c_cc[19]; // 控制字符 (VINTR, VQUIT, VERASE, VEOF...)
    };

    void set_termios(const Termios* t);
    void get_termios(Termios* t) const;

private:
    Termios termios_;
    ByteRingBuffer<4096> input_buf_;  // 原始输入
    ByteRingBuffer<4096> line_buf_;   // 行规范处理后的行
    Console* console_;                // 输出目标
    Task* foreground_group_;          // 前台进程组
};
```

- [ ] TTY 类实现
- [ ] Termios 配置（默认：ICANON + ECHO + ISIG）
- [ ] input_char() 处理特殊字符：
  - Ctrl+C (VINTR) → SIGINT 到前台进程组
  - Ctrl+Z (VSUSP) → SIGTSTP
  - Ctrl+D (VEOF) → EOF
  - Backspace (VERASE) → 删除字符 + 回显

### T2: 行规范 (Line Discipline)

```cpp
void TTY::input_char(char c) {
    if (termios_.c_lflag & ISIG) {
        if (c == termios_.c_cc[VINTR]) {
            // 发送 SIGINT 到前台进程组
            killpg(foreground_group_->pgid, Signal::SIGINT);
            return;
        }
    }

    if (termios_.c_lflag & ICANON) {
        // 规范模式：累积到换行符
        if (c == '\n' || c == termios_.c_cc[VEOF]) {
            // 将整行推入 line_buf_
        } else {
            input_buf_.push(c);
        }
    } else {
        // 原始模式：直接推入
        line_buf_.push(c);
    }

    if (termios_.c_lflag & ECHO) {
        console_->putc(c);  // 回显
    }
}
```

- [ ] 规范模式 (ICANON)：行编辑 + 换行提交
- [ ] 原始模式 (~ICANON)：字符直通
- [ ] 回显控制 (ECHO)
- [ ] 信号生成 (ISIG)

### T3: 伪终端 (PTY)

**文件**: `kernel/drivers/tty/pty.hpp`

```cpp
class PTY {
public:
    // 创建 PTY 对
    static PTY* create();

    // Master 端（终端模拟器使用）
    int master_write(const char* buf, size_t len);  // 模拟器输出 → slave
    int master_read(char* buf, size_t len);          // 读取 slave 输出

    // Slave 端（Shell 使用）
    int slave_read(char* buf, size_t len);           // 读取 master 输入
    int slave_write(const char* buf, size_t len);    // Shell 输出 → master

    TTY* tty();  // 获取关联的 TTY

private:
    TTY tty_;
    ByteRingBuffer<4096> master_to_slave_;
    ByteRingBuffer<4096> slave_to_master_;
};
```

- [ ] PTY master/slave 对创建
- [ ] /dev/ptmx — 获取 master fd
- [ ] /dev/pts/N — slave 端设备文件
- [ ] master 写入 → TTY 行规范 → slave 读取
- [ ] slave 写入 → master 读取

### T4: ioctl Syscall

```cpp
int64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg);
```

支持的 cmd：
- TCGETS / TCSETS — 获取/设置 termios
- TIOCGPGRP / TIOCSPGRP — 获取/设置前台进程组
- TIOCSCTTY — 设置控制终端

- [ ] ioctl syscall 框架
- [ ] TTY 相关 ioctl 命令
- [ ] libc ioctl() wrapper

### T5: 集成

- [ ] 键盘输入 → 当前进程的控制 TTY
- [ ] /dev/console → 系统控制台 TTY
- [ ] /dev/tty → 当前进程的控制终端别名
- [ ] Shell 通过 TTY 读取输入 + 接收信号

## 产出物

- [ ] `kernel/drivers/tty/tty.hpp` / `.cpp` — TTY + 行规范
- [ ] `kernel/drivers/tty/pty.hpp` / `.cpp` — 伪终端
- [ ] ioctl syscall
- [ ] /dev/ptmx + /dev/pts/N
- [ ] 键盘 → TTY 集成
