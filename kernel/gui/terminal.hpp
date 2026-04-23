/**
 * @file kernel/gui/terminal.hpp
 * @brief Terminal window with 80x25 character buffer, cursor, and text rendering
 *
 * A GUI Terminal that inherits from Window and provides an 80x25 text
 * buffer with per-cell foreground/background colours, a blinking cursor,
 * and basic ANSI escape sequence support (clear screen, cursor home,
 * clear to end of line).  Used as the primary text output surface for
 * the Cinux shell and other console programs.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <stdint.h>

#include "kernel/gui/window.hpp"

namespace cinux::drivers { class PSFFont; }
namespace cinux::ipc { class Pipe; }

namespace cinux::gui {

// ============================================================
// TerminalCell -- one character cell in the terminal buffer
// ============================================================

/**
 * @brief Represents a single character cell in the terminal buffer
 */
struct TerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;  // white
    uint32_t bg = 0x00000000;  // black
};

// ============================================================
// Terminal class
// ============================================================

/**
 * @brief A terminal window with 80x25 character buffer and cursor management
 *
 * Provides text rendering via a screen buffer, keyboard input handling
 * through on_key(), and paint output via on_paint().  Supports basic
 * ANSI escape sequences for clear screen, cursor home, and clear-to-EOL.
 *
 * The write() method is intended as the stdout callback from user-space
 * programs (e.g. the shell), allowing their output to be displayed in
 * the terminal window.
 */
class Terminal : public Window {
public:
    // ============================================================
    // Constants
    // ============================================================

    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;

    // ============================================================
    // Construction / destruction
    // ============================================================

    /**
     * @brief Construct a terminal window
     *
     * @param x      Screen X position (pixels)
     * @param y      Screen Y position (pixels)
     * @param title  Window title
     */
    Terminal(uint32_t x, uint32_t y, const char* title = "Cinux Terminal");
    /**
     * @brief Destroy the terminal and close connected pipes
     *
     * Closes stdin pipe write end (notifies shell: EOF on stdin) and
     * stdout pipe read end (shell writes will return -1).  This allows
     * the shell to detect the terminal closure and exit gracefully.
     */
    ~Terminal() override;

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // ============================================================
    // Window virtual overrides
    // ============================================================

    /**
     * @brief Handle a keyboard event
     *
     * Writes the key's ASCII character into the screen buffer.
     * Also forwards the character to the pipe write fd if set
     * (for Phase 5 shell integration).
     *
     * @param ev  The keyboard event
     */
    void on_key(KeyEvent& ev) override;

    /**
     * @brief Paint the terminal content onto a canvas
     *
     * Draws the title bar, fills the content background, renders
     * each non-space character via canvas.draw_text(), and draws
     * the cursor as a reverse-colour block.
     *
     * @param canvas  The canvas to paint onto (the screen canvas)
     */
    void on_paint(cinux::drivers::Canvas& canvas) override;

    // ============================================================
    // External write interface (shell stdout callback)
    // ============================================================

    /**
     * @brief Write a string into the terminal buffer
     *
     * Processes each character, handling printable chars, newlines,
     * carriage returns, backspaces, tabs, and ANSI escape sequences.
     *
     * @param str  String data
     * @param len  Number of bytes to write
     */
    void write(const char* str, uint64_t len);

    // ============================================================
    // Query
    // ============================================================

    /**
     * @brief Access a single cell in the terminal buffer
     *
     * @param row  Row index (0..ROWS-1)
     * @param col  Column index (0..COLS-1)
     * @return     Reference to the cell
     */
    const TerminalCell& cell(uint32_t row, uint32_t col) const;

    /**
     * @brief Get the current cursor column
     * @return cursor X position (0..COLS-1)
     */
    uint32_t cursor_x() const;

    /**
     * @brief Get the current cursor row
     * @return cursor Y position (0..ROWS-1)
     */
    uint32_t cursor_y() const;

    // ============================================================
    // Pipe fd (Phase 5 integration)
    // ============================================================

    /**
     * @brief Set the pipe write file descriptor for shell input
     * @param fd  File descriptor (-1 if not connected)
     */
    void set_pipe_write_fd(int fd);

    /**
     * @brief Get the pipe write file descriptor
     * @return File descriptor, or -1 if not connected
     */
    int pipe_write_fd() const;

    // ============================================================
    // Stdout pipe polling (Phase 5 shell output forwarding)
    // ============================================================

    /**
     * @brief Set the stdin pipe for forwarding keyboard input to shell
     *
     * The Terminal will call try_write() on this pipe during on_key()
     * to send typed characters to the shell's stdin.
     *
     * @param pipe  Pointer to the stdin Pipe (must remain valid)
     */
    void set_stdin_pipe(class cinux::ipc::Pipe* pipe);

    /**
     * @brief Set the stdout pipe for non-blocking output polling
     *
     * The Terminal will call try_read() on this pipe during
     * poll_output() to retrieve shell output and display it.
     *
     * @param pipe  Pointer to the stdout Pipe (must remain valid)
     */
    void set_stdout_pipe(class cinux::ipc::Pipe* pipe);

    /**
     * @brief Non-blocking poll of the stdout pipe for shell output
     *
     * Reads available bytes from the stdout pipe (if set) via
     * try_read() and writes them into the terminal buffer via
     * write().  Should be called from the GUI tick callback.
     */
    void poll_output();

    // ============================================================
    // Rendering
    // ============================================================

    void set_font(cinux::drivers::PSFFont* font);
    void render_to_canvas();

    // ============================================================
    // Clear
    // ============================================================

    /**
     * @brief Clear the entire screen buffer and reset cursor to (0, 0)
     */
    void clear();

private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Process a single character into the screen buffer
     * @param ch  The character to process
     */
    void process_char(char ch);

    /**
     * @brief Scroll the screen buffer up by one line
     *
     * Moves rows 1..ROWS-1 to rows 0..ROWS-2 and clears the last row.
     */
    void scroll_up();

    /**
     * @brief Advance to the beginning of the next line
     *
     * Wraps to the next row; scrolls up if at the bottom.
     */
    void newline();

    /**
     * @brief Handle backspace: erase previous character
     */
    void backspace();

    /**
     * @brief Handle tab: advance cursor to next 8-column tab stop
     */
    void tab();

    // ============================================================
    // ANSI escape sequence state
    // ============================================================

    /**
     * @brief Check if the character starts an ANSI escape sequence
     * @return true if ch == '\033'
     */
    static bool is_escape(char ch);

    /**
     * @brief Try to parse and handle an ANSI CSI sequence
     *
     * Reads from str[pos] until a complete sequence is found or
     * parsing fails.  Advances pos past the consumed bytes.
     *
     * @param str  The source string
     * @param len  Total length of the string
     * @param pos  Current position (updated on return)
     */
    void handle_ansi(const char* str, uint64_t len, uint64_t& pos);

    // ============================================================
    // Members
    // ============================================================

    TerminalCell screen_[ROWS][COLS];
    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t fg_ = 0x00FFFFFF;
    uint32_t bg_ = 0x00000000;
    bool cursor_visible_ = true;
    int pipe_write_fd_ = -1;
    cinux::drivers::PSFFont* font_ = nullptr;
    ipc::Pipe* stdin_pipe_ = nullptr;
    ipc::Pipe* stdout_pipe_ = nullptr;
};

}  // namespace cinux::gui
