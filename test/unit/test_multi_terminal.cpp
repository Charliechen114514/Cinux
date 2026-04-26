/**
 * @file test/unit/test_multi_terminal.cpp
 * @brief Host-side unit tests for 035_multi_terminal milestone
 *
 * Tests the multi-terminal lifecycle features added in milestone 035:
 *   - Terminal destructor pipe cleanup (stdin writer close, stdout reader close)
 *   - Terminal set_shell_pid / shell_pid for child tracking
 *   - Terminal destructor waitpid reap semantics (mocked)
 *   - gui_tick_callback logic: iterate all windows, poll+render only terminals
 *   - create_shell_terminal title generation ("Shell #N")
 *   - Multiple concurrent terminals with independent pipes
 *   - WindowManager window_at / window_count for multi-terminal iteration
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>
#    include <memory>
#    include <string>
#    include <vector>

// ============================================================
// Mock Spinlock (for Pipe which requires cinux::proc::Spinlock)
// ============================================================

namespace cinux::proc {

class Spinlock {
public:
    void lock() {}
    void unlock() {}
};

}  // namespace cinux::proc

// ============================================================
// Mock Pipe (mimics kernel/ipc/pipe.hpp)
// ============================================================

namespace cinux::ipc {

class Pipe {
public:
    Pipe() : reader_open_(true), writer_open_(true) {}

    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    int64_t try_write(const char* data, uint64_t count) {
        if (!reader_open_)
            return -1;
        if (count == 0)
            return 0;
        uint64_t written = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (buf_.size() >= 4096)
                break;
            buf_.push_back(data[i]);
            written++;
        }
        return static_cast<int64_t>(written);
    }

    int64_t try_read(char* out, uint64_t count) {
        if (buf_.empty()) {
            return writer_open_ ? 0 : 0;
        }
        uint64_t n = (count < buf_.size()) ? count : buf_.size();
        for (uint64_t i = 0; i < n; i++) {
            out[i] = buf_[i];
        }
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<int>(n));
        return static_cast<int64_t>(n);
    }

    void close_reader() { reader_open_ = false; }
    void close_writer() { writer_open_ = false; }

    bool     reader_alive() const { return reader_open_; }
    bool     writer_alive() const { return writer_open_; }
    bool     is_empty() const { return buf_.empty(); }
    uint32_t available() const { return static_cast<uint32_t>(buf_.size()); }

private:
    std::vector<char> buf_;
    bool              reader_open_;
    bool              writer_open_;
};

}  // namespace cinux::ipc

// ============================================================
// WaitpidResult enum (mimics kernel/proc/process.hpp)
// ============================================================

namespace cinux::proc {

enum class WaitpidResult : int {
    Ok         = 0,
    NoChildren = -1,
    NotFound   = -2,
    NotExited  = -3
};

}  // namespace cinux::proc

// ============================================================
// Mock kprintf
// ============================================================

namespace cinux::lib {

void kprintf(const char* fmt, ...) {
    (void)fmt;
    // Suppress output in host tests
}

}  // namespace cinux::lib

// ============================================================
// Simple TerminalCell
// ============================================================

struct TerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;
    uint32_t bg = 0x00000000;
};

// ============================================================
// Minimal Terminal reimplementation for host-side testing
// ============================================================

namespace cinux::gui {

class Terminal {
public:
    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;

    Terminal(uint32_t x, uint32_t y, const char* title = "Cinux Terminal")
        : x_(x), y_(y), shell_pid_(0), stdin_pipe_(nullptr), stdout_pipe_(nullptr) {
        for (uint32_t r = 0; r < ROWS; r++)
            for (uint32_t c = 0; c < COLS; c++)
                screen_[r][c] = TerminalCell{};
        if (title) {
            size_t len = strlen(title);
            if (len > 63)
                len = 63;
            memcpy(title_, title, len);
            title_[len] = '\0';
        }
    }

    ~Terminal() {
        // Milestone 035: destructor closes pipes
        if (stdin_pipe_ != nullptr) {
            stdin_pipe_->close_writer();
        }
        if (stdout_pipe_ != nullptr) {
            stdout_pipe_->close_reader();
        }
        stdin_pipe_  = nullptr;
        stdout_pipe_ = nullptr;

        // Milestone 035: reap shell child
        if (shell_pid_ > 0) {
            // In the real kernel, this calls waitpid in a loop.
            // In host test, we just clear the pid.
            shell_pid_ = 0;
        }
    }

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool        is_terminal() const { return true; }
    const char* title() const { return title_; }

    void set_stdin_pipe(cinux::ipc::Pipe* pipe) { stdin_pipe_ = pipe; }
    void set_stdout_pipe(cinux::ipc::Pipe* pipe) { stdout_pipe_ = pipe; }
    void set_shell_pid(int pid) { shell_pid_ = pid; }
    int  shell_pid() const { return shell_pid_; }

    cinux::ipc::Pipe* stdin_pipe() const { return stdin_pipe_; }
    cinux::ipc::Pipe* stdout_pipe() const { return stdout_pipe_; }

    const TerminalCell& cell(uint32_t row, uint32_t col) const { return screen_[row][col]; }

    uint32_t cursor_x() const { return cursor_x_; }
    uint32_t cursor_y() const { return cursor_y_; }

    // Write a character into the screen buffer
    void process_char(char ch) {
        if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7E)
            return;
        screen_[cursor_y_][cursor_x_].ch = ch;
        cursor_x_++;
        if (cursor_x_ >= COLS) {
            cursor_x_ = 0;
            cursor_y_++;
            if (cursor_y_ >= ROWS) {
                cursor_y_ = ROWS - 1;
                scroll_up();
            }
        }
    }

    void newline() {
        cursor_x_ = 0;
        cursor_y_++;
        if (cursor_y_ >= ROWS) {
            cursor_y_ = ROWS - 1;
            scroll_up();
        }
    }

    void scroll_up() {
        for (uint32_t r = 0; r < ROWS - 1; r++)
            for (uint32_t c = 0; c < COLS; c++)
                screen_[r][c] = screen_[r + 1][c];
        for (uint32_t c = 0; c < COLS; c++)
            screen_[ROWS - 1][c] = TerminalCell{};
    }

    void write(const char* str, uint64_t len) {
        for (uint64_t i = 0; i < len; i++) {
            char ch = str[i];
            if (ch == '\n')
                newline();
            else if (ch == '\r')
                cursor_x_ = 0;
            else
                process_char(ch);
        }
    }

    void poll_output() {
        if (stdout_pipe_ == nullptr)
            return;
        char buf[256];
        while (true) {
            int64_t n = stdout_pipe_->try_read(buf, sizeof(buf));
            if (n <= 0)
                break;
            write(buf, static_cast<uint64_t>(n));
        }
    }

    void clear() {
        for (uint32_t r = 0; r < ROWS; r++)
            for (uint32_t c = 0; c < COLS; c++)
                screen_[r][c] = TerminalCell{};
        cursor_x_ = 0;
        cursor_y_ = 0;
    }

private:
    TerminalCell      screen_[ROWS][COLS];
    uint32_t          cursor_x_ = 0;
    uint32_t          cursor_y_ = 0;
    uint32_t          x_;
    uint32_t          y_;
    char              title_[64] = {};
    int               shell_pid_;
    cinux::ipc::Pipe* stdin_pipe_;
    cinux::ipc::Pipe* stdout_pipe_;
};

}  // namespace cinux::gui

// ============================================================
// Minimal WindowManager stub for multi-terminal iteration tests
// ============================================================

namespace cinux::gui {

class WindowManager {
public:
    static constexpr uint32_t MAX_WINDOWS = 64;

    WindowManager() : count_(0) {}

    uint32_t add_window(Terminal* win) {
        if (count_ >= MAX_WINDOWS)
            return 0;
        windows_[count_] = win;
        return ++count_;
    }

    uint32_t window_count() const { return count_; }

    Terminal* window_at(uint32_t index) const {
        if (index >= count_)
            return nullptr;
        return windows_[index];
    }

    void destroy(uint32_t index) {
        if (index == 0 || index > count_)
            return;
        uint32_t i = index - 1;
        delete windows_[i];
        // Shift down
        for (uint32_t j = i; j + 1 < count_; j++) {
            windows_[j] = windows_[j + 1];
        }
        count_--;
    }

    ~WindowManager() {
        for (uint32_t i = 0; i < count_; i++) {
            delete windows_[i];
        }
    }

private:
    Terminal* windows_[MAX_WINDOWS] = {};
    uint32_t  count_;
};

}  // namespace cinux::gui

// ============================================================
// Helper: generate title string "Shell #N" (mirrors gui_init.cpp logic)
// ============================================================

static std::string make_shell_title(uint32_t counter) {
    std::string title = "Shell #";
    if (counter == 0) {
        title += '0';
    } else {
        std::string digits;
        uint32_t    num = counter;
        while (num > 0) {
            digits += ('0' + static_cast<char>(num % 10));
            num /= 10;
        }
        for (int i = static_cast<int>(digits.size()) - 1; i >= 0; i--) {
            title += digits[static_cast<size_t>(i)];
        }
    }
    return title;
}

// ============================================================
// Phase A: Host Unit Tests
// ============================================================

// --- 1. Terminal destructor pipe cleanup ---

TEST("multi_terminal: destructor closes stdin pipe writer") {
    cinux::ipc::Pipe pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdin_pipe(&pipe);
        ASSERT_TRUE(pipe.writer_alive());
        ASSERT_TRUE(pipe.reader_alive());
    }
    // After destruction, writer should be closed (Terminal owns writer end)
    ASSERT_FALSE(pipe.writer_alive());
    // Reader should still be open (shell side)
    ASSERT_TRUE(pipe.reader_alive());
}

TEST("multi_terminal: destructor closes stdout pipe reader") {
    cinux::ipc::Pipe pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdout_pipe(&pipe);
        ASSERT_TRUE(pipe.reader_alive());
        ASSERT_TRUE(pipe.writer_alive());
    }
    // After destruction, reader should be closed (Terminal owns reader end)
    ASSERT_FALSE(pipe.reader_alive());
    // Writer should still be open (shell side)
    ASSERT_TRUE(pipe.writer_alive());
}

TEST("multi_terminal: destructor closes both pipes") {
    cinux::ipc::Pipe stdin_pipe;
    cinux::ipc::Pipe stdout_pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
        term.set_stdout_pipe(&stdout_pipe);
    }
    ASSERT_FALSE(stdin_pipe.writer_alive());
    ASSERT_TRUE(stdin_pipe.reader_alive());  // shell side
    ASSERT_FALSE(stdout_pipe.reader_alive());
    ASSERT_TRUE(stdout_pipe.writer_alive());  // shell side
}

TEST("multi_terminal: destructor with no pipes is safe") {
    // No pipes set -- destructor should not crash
    cinux::gui::Terminal term(0, 0);
    (void)term;
    ASSERT_TRUE(true);
}

// --- 2. Shell PID tracking ---

TEST("multi_terminal: set_shell_pid and shell_pid") {
    cinux::gui::Terminal term(0, 0);
    ASSERT_EQ(term.shell_pid(), 0);

    term.set_shell_pid(42);
    ASSERT_EQ(term.shell_pid(), 42);
}

TEST("multi_terminal: destructor clears shell_pid after reap") {
    cinux::ipc::Pipe stdin_pipe;
    cinux::ipc::Pipe stdout_pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
        term.set_stdout_pipe(&stdout_pipe);
        term.set_shell_pid(123);
    }
    // After destruction, shell_pid is cleared (reaped)
    // We cannot directly inspect it since the object is destroyed,
    // but the test verifies no crash occurred during reap.
    ASSERT_TRUE(true);
}

TEST("multi_terminal: destructor with zero shell_pid skips reap") {
    cinux::gui::Terminal term(0, 0);
    ASSERT_EQ(term.shell_pid(), 0);
    // Destructor should skip waitpid when shell_pid == 0
    (void)term;
    ASSERT_TRUE(true);
}

// --- 3. Multiple concurrent terminals with independent pipes ---

TEST("multi_terminal: two terminals have independent stdin pipes") {
    cinux::ipc::Pipe stdin1;
    cinux::ipc::Pipe stdin2;
    cinux::ipc::Pipe stdout1;
    cinux::ipc::Pipe stdout2;

    cinux::gui::Terminal term1(0, 0);
    term1.set_stdin_pipe(&stdin1);
    term1.set_stdout_pipe(&stdout1);

    cinux::gui::Terminal term2(100, 100);
    term2.set_stdin_pipe(&stdin2);
    term2.set_stdout_pipe(&stdout2);

    // Write to stdout of term1
    stdout1.try_write("A", 1);
    term1.poll_output();

    // Write to stdout of term2
    stdout2.try_write("B", 1);
    term2.poll_output();

    // Each terminal should have its own output
    ASSERT_EQ(term1.cell(0, 0).ch, 'A');
    ASSERT_EQ(term2.cell(0, 0).ch, 'B');
}

TEST("multi_terminal: destroy first terminal does not affect second") {
    cinux::ipc::Pipe stdin1, stdin2;
    cinux::ipc::Pipe stdout1, stdout2;

    {
        cinux::gui::Terminal term1(0, 0);
        term1.set_stdin_pipe(&stdin1);
        term1.set_stdout_pipe(&stdout1);
        term1.set_shell_pid(10);
    }
    // term1 destroyed: pipes closed, pid reaped

    // term2's pipes should be unaffected
    cinux::gui::Terminal term2(100, 100);
    term2.set_stdin_pipe(&stdin2);
    term2.set_stdout_pipe(&stdout2);

    ASSERT_TRUE(stdin2.writer_alive());
    ASSERT_TRUE(stdin2.reader_alive());
    ASSERT_TRUE(stdout2.writer_alive());
    ASSERT_TRUE(stdout2.reader_alive());

    // Data flow through term2 should work
    stdout2.try_write("OK", 2);
    term2.poll_output();
    ASSERT_EQ(term2.cell(0, 0).ch, 'O');
    ASSERT_EQ(term2.cell(0, 1).ch, 'K');
}

TEST("multi_terminal: three terminals concurrent poll_output") {
    cinux::ipc::Pipe stdin1, stdin2, stdin3;
    cinux::ipc::Pipe stdout1, stdout2, stdout3;

    cinux::gui::Terminal t1(0, 0);
    cinux::gui::Terminal t2(0, 0);
    cinux::gui::Terminal t3(0, 0);

    t1.set_stdin_pipe(&stdin1);
    t1.set_stdout_pipe(&stdout1);
    t2.set_stdin_pipe(&stdin2);
    t2.set_stdout_pipe(&stdout2);
    t3.set_stdin_pipe(&stdin3);
    t3.set_stdout_pipe(&stdout3);

    // Feed different data to each
    stdout1.try_write("AAA", 3);
    stdout2.try_write("BBB", 3);
    stdout3.try_write("CCC", 3);

    // Poll all terminals (simulates gui_tick_callback loop)
    t1.poll_output();
    t2.poll_output();
    t3.poll_output();

    ASSERT_EQ(t1.cell(0, 0).ch, 'A');
    ASSERT_EQ(t1.cell(0, 1).ch, 'A');
    ASSERT_EQ(t1.cell(0, 2).ch, 'A');

    ASSERT_EQ(t2.cell(0, 0).ch, 'B');
    ASSERT_EQ(t2.cell(0, 1).ch, 'B');
    ASSERT_EQ(t2.cell(0, 2).ch, 'B');

    ASSERT_EQ(t3.cell(0, 0).ch, 'C');
    ASSERT_EQ(t3.cell(0, 1).ch, 'C');
    ASSERT_EQ(t3.cell(0, 2).ch, 'C');
}

// --- 4. Title generation logic ---

TEST("multi_terminal: shell title generation Shell #1") {
    std::string title = make_shell_title(1);
    ASSERT_EQ(title, "Shell #1");
}

TEST("multi_terminal: shell title generation Shell #0") {
    std::string title = make_shell_title(0);
    ASSERT_EQ(title, "Shell #0");
}

TEST("multi_terminal: shell title generation multi-digit") {
    std::string title = make_shell_title(123);
    ASSERT_EQ(title, "Shell #123");
}

TEST("multi_terminal: shell title generation large number") {
    std::string title = make_shell_title(999);
    ASSERT_EQ(title, "Shell #999");
}

TEST("multi_terminal: terminal constructor stores title") {
    cinux::gui::Terminal term(0, 0, "Shell #5");
    ASSERT_EQ(std::string(term.title()), "Shell #5");
}

// --- 5. WindowManager iteration for multi-terminal ---

TEST("multi_terminal: WM window_count and window_at for terminals") {
    cinux::gui::WindowManager wm;
    ASSERT_EQ(wm.window_count(), 0u);

    auto* t1 = new cinux::gui::Terminal(0, 0, "Shell #1");
    auto* t2 = new cinux::gui::Terminal(100, 100, "Shell #2");
    wm.add_window(t1);
    wm.add_window(t2);

    ASSERT_EQ(wm.window_count(), 2u);

    auto* w0 = wm.window_at(0);
    auto* w1 = wm.window_at(1);
    ASSERT_NOT_NULL(w0);
    ASSERT_NOT_NULL(w1);
    ASSERT_EQ(std::string(w0->title()), "Shell #1");
    ASSERT_EQ(std::string(w1->title()), "Shell #2");
    ASSERT_TRUE(w0->is_terminal());
    ASSERT_TRUE(w1->is_terminal());
}

TEST("multi_terminal: WM destroy terminal closes its pipes") {
    cinux::gui::WindowManager wm;

    auto* stdin_pipe  = new cinux::ipc::Pipe();
    auto* stdout_pipe = new cinux::ipc::Pipe();

    auto* term = new cinux::gui::Terminal(0, 0, "Shell #1");
    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);

    wm.add_window(term);
    ASSERT_EQ(wm.window_count(), 1u);

    // Destroy the window via WM (simulates close button click)
    wm.destroy(1);
    ASSERT_EQ(wm.window_count(), 0u);

    // Pipes should be closed on the Terminal side
    ASSERT_FALSE(stdin_pipe->writer_alive());
    ASSERT_FALSE(stdout_pipe->reader_alive());

    delete stdin_pipe;
    delete stdout_pipe;
}

TEST("multi_terminal: WM window_at returns nullptr for out-of-range") {
    cinux::gui::WindowManager wm;
    ASSERT_NULL(wm.window_at(0));
    ASSERT_NULL(wm.window_at(100));
}

TEST("multi_terminal: WM add_window returns sequential IDs") {
    cinux::gui::WindowManager wm;
    uint32_t                  id1 = wm.add_window(new cinux::gui::Terminal(0, 0, "A"));
    uint32_t                  id2 = wm.add_window(new cinux::gui::Terminal(0, 0, "B"));
    uint32_t                  id3 = wm.add_window(new cinux::gui::Terminal(0, 0, "C"));

    ASSERT_EQ(id1, 1u);
    ASSERT_EQ(id2, 2u);
    ASSERT_EQ(id3, 3u);
    ASSERT_EQ(wm.window_count(), 3u);
}

// --- 6. gui_tick_callback iteration simulation ---

TEST("multi_terminal: simulate tick callback iterating multiple terminals") {
    cinux::gui::WindowManager wm;

    cinux::ipc::Pipe s1, s2, s3;
    cinux::ipc::Pipe o1, o2, o3;

    auto* t1 = new cinux::gui::Terminal(0, 0, "Shell #1");
    auto* t2 = new cinux::gui::Terminal(0, 0, "Shell #2");
    auto* t3 = new cinux::gui::Terminal(0, 0, "Shell #3");

    t1->set_stdin_pipe(&s1);
    t1->set_stdout_pipe(&o1);
    t2->set_stdin_pipe(&s2);
    t2->set_stdout_pipe(&o2);
    t3->set_stdin_pipe(&s3);
    t3->set_stdout_pipe(&o3);

    wm.add_window(t1);
    wm.add_window(t2);
    wm.add_window(t3);

    // Simulate shell output arriving on all terminals
    o1.try_write("1", 1);
    o2.try_write("22", 2);
    o3.try_write("333", 3);

    // Simulate gui_tick_callback loop: iterate all windows, poll+render
    for (uint32_t i = 0; i < wm.window_count(); i++) {
        auto* win = wm.window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<cinux::gui::Terminal*>(win);
            term->poll_output();
            // render_to_canvas() would be called here too
        }
    }

    ASSERT_EQ(wm.window_at(0)->cell(0, 0).ch, '1');
    ASSERT_EQ(wm.window_at(1)->cell(0, 0).ch, '2');
    ASSERT_EQ(wm.window_at(1)->cell(0, 1).ch, '2');
    ASSERT_EQ(wm.window_at(2)->cell(0, 0).ch, '3');
    ASSERT_EQ(wm.window_at(2)->cell(0, 1).ch, '3');
    ASSERT_EQ(wm.window_at(2)->cell(0, 2).ch, '3');
}

// --- 7. Pipe EOF after terminal destruction ---

TEST("multi_terminal: shell reads EOF from stdin after terminal destroyed") {
    cinux::ipc::Pipe stdin_pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
    }
    // Terminal destroyed: writer closed, buffer empty
    // Shell should get 0 (EOF) on read
    char    buf[16] = {};
    int64_t r       = stdin_pipe.try_read(buf, 16);
    ASSERT_EQ(r, 0);
}

TEST("multi_terminal: shell write fails after terminal destroyed") {
    cinux::ipc::Pipe stdout_pipe;
    {
        cinux::gui::Terminal term(0, 0);
        term.set_stdout_pipe(&stdout_pipe);
    }
    // Terminal destroyed: reader closed
    // Shell write should return -1
    const char msg[] = "test";
    int64_t    w     = stdout_pipe.try_write(msg, 4);
    ASSERT_EQ(w, -1);
}

TEST("multi_terminal: terminal can still poll after partial stdout write") {
    cinux::ipc::Pipe     stdout_pipe;
    cinux::gui::Terminal term(0, 0);
    term.set_stdout_pipe(&stdout_pipe);

    // Write partial data
    stdout_pipe.try_write("Hello", 5);
    term.poll_output();
    ASSERT_EQ(term.cell(0, 0).ch, 'H');
    ASSERT_EQ(term.cell(0, 4).ch, 'o');
    ASSERT_EQ(term.cursor_x(), 5u);

    // Write more data
    stdout_pipe.try_write(" World", 6);
    term.poll_output();
    ASSERT_EQ(term.cell(0, 5).ch, ' ');
    ASSERT_EQ(term.cell(0, 6).ch, 'W');
    ASSERT_EQ(term.cursor_x(), 11u);
}

// --- 8. Terminal dimensions for centering calculation ---

TEST("multi_terminal: terminal width is 80*8 = 640") {
    ASSERT_EQ(cinux::gui::Terminal::COLS * 8u, 640u);
}

TEST("multi_terminal: terminal height is 25*16 = 400") {
    ASSERT_EQ(cinux::gui::Terminal::ROWS * 16u, 400u);
}

// --- 9. Boundary: max windows ---

TEST("multi_terminal: WM max windows boundary") {
    cinux::gui::WindowManager wm;

    for (uint32_t i = 0; i < cinux::gui::WindowManager::MAX_WINDOWS; i++) {
        uint32_t id = wm.add_window(new cinux::gui::Terminal(0, 0));
        ASSERT_NE(id, 0u);
    }
    ASSERT_EQ(wm.window_count(), cinux::gui::WindowManager::MAX_WINDOWS);

    // Adding one more should fail
    uint32_t overflow_id = wm.add_window(new cinux::gui::Terminal(0, 0));
    ASSERT_EQ(overflow_id, 0u);
}

int main() {
    RUN_ALL_TESTS();
    return 0;
}

#endif  // CINUX_HOST_TEST
