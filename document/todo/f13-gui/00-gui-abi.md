# M1: Kernel-GUI ABI 接口定义

> 定义 GUI 独立仓库可依赖的稳定接口集合。
> 纯头文件，不暴露任何内核内部类型。

## 设计原则

1. **C linkage** — `extern "C"` 接口，避免 C++ ABI 不稳定
2. **不透明句柄** — 用 `struct GuiXxx*` 替代内核内部类型
3. **所有权明确** — 每个 API 注明谁分配、谁释放
4. **无全局状态** — GUI 持有 context 结构，内核不持有 GUI 指针

## ABI 接口定义

**文件**: `kernel/gui/gui_abi.hpp`

### 1. 渲染接口

```c
// 不透明类型
typedef struct GuiFramebuffer GuiFramebuffer;
typedef struct GuiCanvas GuiCanvas;

// Framebuffer（硬件屏幕）
GuiFramebuffer* gui_get_framebuffer();
uint32_t gui_fb_width(GuiFramebuffer* fb);
uint32_t gui_fb_height(GuiFramebuffer* fb);
uint32_t gui_fb_pitch(GuiFramebuffer* fb);
uint32_t* gui_fb_data(GuiFramebuffer* fb);  // 直接像素访问

// Canvas（离屏缓冲区）
GuiCanvas* gui_canvas_create(uint32_t width, uint32_t height);  // GUI 分配
void gui_canvas_destroy(GuiCanvas* canvas);                      // GUI 释放
void gui_canvas_clear(GuiCanvas* canvas, uint32_t color);
void gui_canvas_draw_pixel(GuiCanvas* c, uint32_t x, uint32_t y, uint32_t color);
void gui_canvas_draw_rect(GuiCanvas* c, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void gui_canvas_draw_text(GuiCanvas* c, uint32_t x, uint32_t y, const char* text, uint32_t color);
void gui_canvas_blit(GuiCanvas* dst, uint32_t dx, uint32_t dy,
                     GuiCanvas* src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
void gui_canvas_flip(GuiCanvas* canvas);  // 前后缓冲交换
```

### 2. 字体接口

```c
typedef struct GuiFont GuiFont;

GuiFont* gui_get_default_font();
uint32_t gui_font_width(GuiFont* font);
uint32_t gui_font_height(GuiFont* font);
const uint8_t* gui_font_glyph(GuiFont* font, uint32_t codepoint);
```

### 3. 输入事件接口

```c
// 事件类型（与 GUI 仓库约定）
typedef enum GuiEventType {
    GUI_EVENT_MOUSE_MOVE = 0,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
} GuiEventType;

typedef struct GuiMouseEvent {
    int32_t x, y;
    int32_t dx, dy;
    uint8_t buttons;  // bit 0=left, 1=right, 2=middle
} GuiMouseEvent;

typedef struct GuiKeyEvent {
    char ascii;
    uint8_t scancode;
    bool pressed;
    bool shift, ctrl, alt;
} GuiKeyEvent;

typedef struct GuiEvent {
    GuiEventType type;
    union {
        GuiMouseEvent mouse;
        GuiKeyEvent key;
    };
} GuiEvent;

// 事件队列（GUI 消费端）
bool gui_event_dequeue(GuiEvent* out_event);  // 非阻塞
void gui_event_clear();
```

### 4. Shell 集成接口

```c
typedef struct GuiShell GuiShell;  // 不透明 Shell 会话

// 启动 Shell 进程，返回会话句柄
// GUI 负责：创建 Pipe、调用此函数、传入 pipe 端
// 内核负责：fork + execve + 设置 stdin/stdout
GuiShell* gui_shell_spawn(const char* path);

// 向 Shell stdin 写入数据（键盘输入转发）
int gui_shell_write_stdin(GuiShell* shell, const char* data, size_t len);

// 从 Shell stdout 读取数据（Shell 输出获取）
int gui_shell_read_stdout(GuiShell* shell, char* buf, size_t len);

// 检查 Shell 是否存活
bool gui_shell_is_alive(GuiShell* shell);

// 等待 Shell 退出并获取状态
int gui_shell_wait(GuiShell* shell);

// 销毁 Shell 会话（关闭 pipe、回收资源）
void gui_shell_destroy(GuiShell* shell);
```

### 5. 定时器接口

```c
// 注册周期回调（用于 GUI 刷新）
// interval_ms: 回调间隔（毫秒）
// callback: 回调函数
// ctx: 透传上下文
typedef void (*GuiTickCallback)(void* ctx);
void gui_register_tick(uint32_t interval_ms, GuiTickCallback callback, void* ctx);
void gui_unregister_tick();
```

### 6. 内存分配接口

```c
// GUI 可以使用内核堆分配
void* gui_alloc(size_t size);
void gui_free(void* ptr);
```

### 7. 日志接口

```c
// 调试日志（转发到 kprintf）
void gui_log(const char* fmt, ...);
```

### 8. 鼠标光标状态

```c
// 获取/设置鼠标位置（由内核 mouse driver 更新）
int32_t gui_mouse_x();
int32_t gui_mouse_y();
void gui_mouse_set_screen_bounds(int32_t width, int32_t height);
```

## 线程安全约定

| 接口 | 线程安全 | 说明 |
|------|---------|------|
| 渲染 | 单线程 | GUI 刷新在 tick callback 中执行 |
| 事件队列 | SPSC | 内核生产（IRQ）、GUI 消费（tick） |
| Shell | 单线程 | GUI tick 中轮询 stdout |
| 定时器 | 中断上下文 | tick callback 在 IRQ 中执行，必须快速 |
| 内存 | 线程安全 | 内部加锁 |

## ABI 版本

```c
#define GUI_ABI_VERSION 1
uint32_t gui_abi_version();  // 返回 GUI_ABI_VERSION
```

- [ ] 完整 gui_abi.hpp 头文件编写
- [ ] 每个函数添加 Doxygen 注释
- [ ] ABI 版本号约定

## 产出物

- [ ] `kernel/gui/gui_abi.hpp` — 完整 ABI 定义
- [ ] 纯 C 兼容，无内核内部类型依赖
- [ ] 可被独立仓库直接 #include
