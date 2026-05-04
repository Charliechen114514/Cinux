# 031-2: Terminal 组件 — 字符缓冲区、ANSI 解析与 Canvas 渲染

## 概览

本文是 tag 031 通读系列的第二篇，聚焦于 Terminal 组件的完整实现。Terminal 是 Cinux GUI 中最复杂的单个组件——它既是一个 Window 子类（有位置、大小、标题栏），又是一个文本终端模拟器（有字符缓冲区、光标、ANSI 转义序列处理），还是一个管道终端节点（通过 stdin/stdout 管道与 shell 进程通信）。

代码来自 `kernel/gui/terminal.hpp` 和 `kernel/gui/terminal.cpp`，约 700 行。

关键设计决策：Terminal 使用固定大小 80x25 的字符网格而非动态缓冲区，ANSI 支持仅限于 shell 输出中最常用的几个序列，渲染通过逐字符 PSF 位图绘制到 Window 的 Canvas。

## 架构图

```
                  Keyboard IRQ
                       |
                       v
                  Terminal::on_key()
                       |
              +--------+--------+
              |                 |
        stdin_pipe set?    stdin_pipe NULL
              |                 |
        try_write(ch)    process_char(ch)
              |                 |
              v                 v
          stdin Pipe      screen_[y][x]
                              |
                              v
                     Terminal::write()
                              |
                     +--------+--------+
                     |        |        |
                  ESC?     \n \r \b \t  printable
                     |        |        |
                handle_ansi  special  process_char
                                        |
                                        v
                                  screen_[y][x]

Shell Process                        |
   sys_write(fd=1)                   |
        |                            |
   stdout Pipe <---------------------+
        |
  PIT tick -> poll_output() -> try_read -> Terminal::write()
        |
  render_to_canvas() -> Canvas pixels
```

## 代码精讲

### TerminalCell 与字符缓冲区 (terminal.hpp)

```cpp
struct TerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;  // white
    uint32_t bg = 0x00000000;  // black
};
```

每个格子三个字段：字符、前景色（RGBA）、背景色（RGBA）。默认是空格、白字黑底。颜色使用 0x00RRGGBB 格式，和 Canvas 的像素格式一致，渲染时不需要转换。

```cpp
class Terminal : public Window {
public:
    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;

    Terminal(uint32_t x, uint32_t y, const char* title = "Cinux Terminal");
    ~Terminal() override;

    void on_key(KeyEvent& ev) override;
    void on_paint(cinux::drivers::Canvas& canvas) override;
    bool is_terminal() const override { return true; }

    void write(const char* str, uint64_t len);
    void set_stdin_pipe(class cinux::ipc::Pipe* pipe);
    void set_stdout_pipe(class cinux::ipc::Pipe* pipe);
    void set_shell_pid(int pid);
    void poll_output();
    void set_font(cinux::drivers::PSFFont* font);
    void render_to_canvas();
    void clear();

private:
    TerminalCell screen_[ROWS][COLS];
    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t fg_ = 0x00FFFFFF;
    uint32_t bg_ = 0x00000000;
    bool cursor_visible_ = true;
    cinux::drivers::PSFFont* font_ = nullptr;
    ipc::Pipe* stdin_pipe_  = nullptr;
    ipc::Pipe* stdout_pipe_ = nullptr;
    int shell_pid_ = 0;
};
```

80x25 是经典 VGA 文本模式的分辨率，也是大多数终端模拟器的默认尺寸。Terminal 继承 Window，构造时根据字符网格大小计算窗口像素尺寸：640 x 400（80 * 8 x 25 * 16）。`is_terminal()` 虚方法让 WindowManager 在 tick 回调中安全地识别终端窗口，避免对普通 Window 做不必要的 static_cast。

### 构造与析构 (terminal.cpp)

```cpp
Terminal::Terminal(uint32_t x, uint32_t y, const char* title)
    : Window(title, static_cast<int32_t>(x), static_cast<int32_t>(y),
             COLS * 8, ROWS * 16) {
    for (uint32_t r = 0; r < ROWS; r++)
        for (uint32_t c = 0; c < COLS; c++)
            screen_[r][c] = TerminalCell{};
}
```

构造函数把窗口的 content area 大小设为 640x400 像素，然后初始化所有格子为默认值。

析构函数负责关闭管道端点和 reap shell 子进程：

```cpp
Terminal::~Terminal() {
    if (stdin_pipe_ != nullptr)  stdin_pipe_->close_writer();
    if (stdout_pipe_ != nullptr) stdout_pipe_->close_reader();
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;

    if (shell_pid_ > 0) {
        for (uint32_t attempt = 0; attempt < 1000; attempt++) {
            int status = 0;
            auto result = cinux::proc::waitpid(shell_pid_, &status,
                                                cinux::proc::g_pid_alloc);
            if (result == cinux::proc::WaitpidResult::Ok) break;
            if (result == cinux::proc::WaitpidResult::NoChildren ||
                result == cinux::proc::WaitpidResult::NotFound) break;
        }
        shell_pid_ = 0;
    }
}
```

管道关闭的方向很重要：Terminal 拥有 stdin 管道的写端（键盘 -> shell），所以关闭写端；拥有 stdout 管道的读端（shell -> 终端），所以关闭读端。关闭后 shell 的 sys_read(fd=0) 会在管道空时返回 EOF，shell 的 sys_write(fd=1) 会收到写端已关闭的错误。waitpid 有限次数尝试，防止在 shell 长时间运行时析构函数永久阻塞。

### 键盘事件处理 (terminal.cpp — on_key)

```cpp
void Terminal::on_key(KeyEvent& ev) {
    if (!ev.pressed) return;
    if (ev.ascii == 0) return;

    if (stdin_pipe_ != nullptr) {
        char ch = ev.ascii;
        if (ch == '\r') ch = '\n';
        stdin_pipe_->try_write(&ch, 1);
        return;
    }
    process_char(ev.ascii);
}
```

这里体现了终端的两种工作模式：连接了 stdin 管道时，键盘输入转发给 shell（echo 由 shell 通过 stdout 回来）；没连接管道时，直接显示到屏幕缓冲区。使用 try_write 而不是阻塞 write 是因为 on_key 可能在中断上下文中被调用。

### 字符写入与处理 (terminal.cpp — write / process_char)

```cpp
void Terminal::write(const char* str, uint64_t len) {
    uint64_t pos = 0;
    while (pos < len) {
        char ch = str[pos];
        if (is_escape(ch)) { handle_ansi(str, len, pos); continue; }
        switch (ch) {
        case '\n': newline(); break;
        case '\r': cursor_x_ = 0; break;
        case '\b': backspace(); break;
        case '\t': tab(); break;
        default:   process_char(ch); break;
        }
        pos++;
    }
}
```

write 是 shell 输出的入口，遍历每个字符分派处理。ESC 字符进入 ANSI 解析流程，控制字符走各自的逻辑，可打印字符走 process_char。

```cpp
void Terminal::process_char(char ch) {
    if (static_cast<uint8_t>(ch) < 0x20 ||
        static_cast<uint8_t>(ch) > 0x7E) return;

    screen_[cursor_y_][cursor_x_].ch = ch;
    screen_[cursor_y_][cursor_x_].fg = fg_;
    screen_[cursor_y_][cursor_x_].bg = bg_;
    cursor_x_++;
    if (cursor_x_ >= COLS) { cursor_x_ = 0; newline(); }
}
```

只处理可打印 ASCII（0x20-0x7E），写入当前光标位置后右移。行末自动换行。

### 滚动与换行 (terminal.cpp — scroll_up / newline)

```cpp
void Terminal::scroll_up() {
    for (uint32_t r = 0; r < ROWS - 1; r++)
        for (uint32_t c = 0; c < COLS; c++)
            screen_[r][c] = screen_[r + 1][c];
    for (uint32_t c = 0; c < COLS; c++)
        screen_[ROWS - 1][c] = TerminalCell{};
}

void Terminal::newline() {
    cursor_x_ = 0;
    cursor_y_++;
    if (cursor_y_ >= ROWS) {
        cursor_y_ = ROWS - 1;
        scroll_up();
    }
}
```

scroll_up 把所有行上移一行然后清空最后一行。这个实现对于每个字符格子的拷贝性能不是最优的（80x25 = 2000 个格子），但在教学 OS 中足够——终端更新的频率远低于 CPU 处理速度。

### ANSI 转义序列解析 (terminal.cpp — handle_ansi)

```cpp
void Terminal::handle_ansi(const char* str, uint64_t len, uint64_t& pos) {
    if (pos + 1 >= len || str[pos + 1] != '[') { pos++; return; }
    pos += 2;
    uint32_t param = 0;
    while (pos < len) {
        char ch = str[pos];
        if (ch >= '0' && ch <= '9') {
            param = param * 10 + static_cast<uint32_t>(ch - '0');
            pos++;
        } else if (ch == ';') {
            pos++;
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            pos++;
            switch (ch) {
            case 'J': if (param == 2) clear(); return;
            case 'H': cursor_x_ = 0; cursor_y_ = 0; return;
            case 'K': /* clear to EOL */ return;
            case 'm': /* SGR: ignore */ return;
            default: return;
            }
        } else { return; }
    }
}
```

解析 CSI（Control Sequence Introducer）序列：ESC [ 参数... 终结字母。参数只收集一个数字（param），多参数场景用分号分隔时目前只取最后一个。支持的命令：J（清除屏幕，仅 param==2）、H（光标归位）、K（清除到行末）、m（SGR，暂忽略）。

这套 ANSI 支持虽然有限，但覆盖了 Cinux shell 输出中最常用的控制序列。shell 的 clear 命令用 ESC[2JESC[H，提示符刷新用 ESC[K 清除旧行。

### 管道输出轮询 (terminal.cpp — poll_output)

```cpp
void Terminal::poll_output() {
    if (stdout_pipe_ == nullptr) return;
    char buf[256];
    while (true) {
        int64_t n = stdout_pipe_->try_read(buf, sizeof(buf));
        if (n <= 0) break;
        write(buf, static_cast<uint64_t>(n));
    }
}
```

poll_output 从 stdout 管道非阻塞地读取数据，然后通过 write 写入字符缓冲区。这个方法在 PIT tick 回调中被周期性调用（大约每秒 100 次），实现 shell 输出到终端显示的实时更新。256 字节的本地缓冲区意味着每次 poll 最多读取 256 字节——对于交互式 shell 来说完全够用。

### Canvas 渲染 (terminal.cpp — render_to_canvas)

```cpp
void Terminal::render_to_canvas() {
    if (font_ == nullptr) return;
    auto& cvs = canvas();
    uint32_t gw = font_->width();
    uint32_t gh = font_->height();

    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            const TerminalCell& cell = screen_[row][col];
            uint32_t px = col * gw;
            uint32_t py = TITLE_BAR_HEIGHT + row * gh;
            cvs.draw_rect(px, py, gw, gh, cell.bg);
            if (cell.ch > ' ') {
                const uint8_t* g = font_->glyph(static_cast<uint8_t>(cell.ch));
                if (g != nullptr) {
                    for (uint32_t gr = 0; gr < gh; gr++) {
                        uint8_t bits = g[gr];
                        for (uint32_t gc = 0; gc < gw; gc++) {
                            if ((bits >> (7 - gc)) & 1)
                                cvs.draw_pixel(px + gc, py + gr, cell.fg);
                        }
                    }
                }
            }
        }
    }
    // cursor rendering (reverse video) ...
}
```

渲染流程是逐格子扫描：先画背景矩形，再按 PSF 字形位图逐像素画前景色。光标用反色块表示——在光标位置画一个前景色矩形，如果该位置有字符则用背景色画字形位图。py 坐标从 TITLE_BAR_HEIGHT 开始，因为前 20 个像素是窗口的标题栏区域。

## 设计决策

### 决策：固定 80x25 字符网格
**问题**: 终端缓冲区用固定大小还是动态大小？
**本项目的做法**: 固定 80x25 数组（2000 个 TerminalCell，约 16KB）。
**备选方案**: 动态 vector 或链表结构，支持窗口 resize 时调整行列数。
**为什么不选备选方案**: 固定大小实现简单，不需要动态内存管理。80x25 是经典 VGA 文本模式尺寸，兼容性好。教学 OS 中窗口 resize 暂时不是优先级。
**如果要扩展/改进**: 支持 resize 时重新分配 screen_ 数组，调整 COLS/ROWS 常量为实例变量。

### 决策：ANSI 序列支持范围
**问题**: 支持多少 ANSI 控制序列？
**本项目的做法**: 仅支持 J（清屏）、H（归位）、K（清行）、m（忽略），不处理光标移动和颜色。
**备选方案**: 完整 VT100/VT220 兼容，支持所有 CSI 序列。
**为什么不选备选方案**: Cinux shell 目前只输出这几种序列。完整 VT100 实现是一个独立大工程，不值得在当前阶段投入。
**如果要扩展/改进**: 增加 ESC[nA/B/C/D（光标移动）、ESC[38;5;n;m（256 色 SGR）、ESC[?25h/l（光标显隐），逐步逼近完整 VT100 兼容。

## 扩展方向

- **终端 resize**: 窗口大小变化时动态调整行列数，重排字符内容 (难度: 较高)
- **完整 SGR 颜色**: 支持 8 色和 256 色前景/背景色 (难度: 中等)
- **选择与复制**: 鼠标拖选文本、Ctrl+Shift+C/V 复制粘贴 (难度: 较高)
- **Scrollback buffer**: 保留滚动出屏幕的历史行，支持 Shift+PageUp 回看 (难度: 中等)
- **Unicode 支持**: UTF-8 解码 + 宽字符渲染 (难度: 较高)

## 参考资料

- Wikipedia: [ANSI escape code](https://en.wikipedia.org/wiki/ANSI_escape_code) — CSI 序列格式和语义定义
- VT100.net: [VT510 Reference Manual](https://vt100.net/docs/vt510-rm/chapter4.html) — DEC VT 系列终端的控制功能规范
- NEU: [ANSI/VT100 Terminal Control](https://www2.ccs.neu.edu/research/gpc/VonaUtils/vona/terminal/vtansi.htm) — 常用 ANSI 控制序列速查
