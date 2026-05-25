---
title: 030-gui-wm-basic-2 · 窗口管理器
---

# 030-2 Read-through: Window + WindowManager

## 概览

本文 walkthrough 覆盖 Window 类和 WindowManager 类的完整实现。Window 封装了一个 GUI 窗口的所有可视元素——标题栏、关闭按钮、内容区、离屏画布。WindowManager 是窗口的"大脑"，负责创建/销毁窗口、维护 Z-序、管理焦点、响应鼠标事件、执行桌面合成。这两个类构成了 Cinux GUI 的核心框架。

关键设计决策：窗口用指针数组管理以避免拷贝，Z-序通过数组索引隐式表达，焦点自动跟随最顶层窗口。

## 架构图

```
                WindowManager (singleton)
                +---------------------------+
                | windows_[0..63]           |  ← Z-ordered pointer array
                |   [0] Window* (bottom)    |
                |   [1] Window*             |
                |   [n] Window* (top)       |  ← focused_
                +---------------------------+
                     |              |
                     v              v
              +------------+  +------------+
              | Window     |  | Window     |
              | id_=1      |  | id_=2      |
              | x_,y_,w_,h |  | x_,y_,w_,h |
              | canvas_    |  | canvas_    |
              | title_[]   |  | title_[]   |
              | visible_   |  | visible_   |
              | focused_   |  | focused_   |
              +------------+  +------------+
                   |                |
                   v                v
              +----------+    +----------+
              | Canvas   |    | Canvas   |
              | (offscr) |    | (offscr) |
              +----------+    +----------+

    composite() flow:
    Screen Canvas ← clear(Desktop Color)
                   ← blit windows[0]
                   ← blit windows[1]
                   ← blit windows[n]   (top-most)
                   ← draw_cursor()
                   ← flip()
```

## 代码精讲

### window.hpp — Window 类声明

Window 的常量定义了视觉规范：标题栏高度 20px，关闭按钮 14x14px，标题最长 63 字符，默认窗口 320x240。颜色方案是硬编码的 0x00RRGGBB 格式——标题栏背景用钢蓝色（0x336699），标题文字白色，关闭按钮红色，内容区浅灰，边框深灰。

构造函数接收标题、位置和尺寸，自动分配唯一 ID。Window 的公共接口都是非虚方法——draw_title_bar、draw_content、blit_to、set_position、resize、set_title、is_close_button_hit、contains，以及各种 getter/setter。没有虚函数，也没有继承层次——tag 030 的 Window 是一个纯粹的值-绘制类。

Window 是 non-copyable 的（拷贝构造和赋值都被 delete），因为每个 Window 独占一个 Canvas（Canvas 持有通过 init(w,h) 分配的 back_buf_ 堆内存）。如果允许拷贝，两个 Window 指向同一个 back_buf_ 会在析构时 double-free。

### window.cpp — Window 实现

构造函数的逻辑很清晰：分配 ID、初始化位置尺寸、手动复制标题字符串（零填充 + 截断），最后调用 allocate_canvas。手动复制而不是用 strncpy 是因为我们的内核没有标准库——这个模式在 Cinux 的很多地方都能看到。

draw_title_bar 分三步：先画蓝色背景矩形（覆盖标题栏整个区域），然后画一行标题文字（垂直居中），最后画红色关闭按钮方块（右上角，左右和上下都留 3px 边距）。底部有一条 1px 的边框线分隔标题栏和内容区。

draw_content 清空内容区为浅灰色（COLOR_CONTENT_BG = 0x00E0E0E0）。在 tag 030 中没有子类扩展点——内容区只是一个空灰色矩形。后续 tag 会引入 Terminal 等子类来在这里渲染自定义内容。

blit_to 是窗口从离屏到屏幕的关键桥梁。它调用目标 Canvas 的 blit 方法，把窗口的整个离屏画布（从 (0,0) 到 (w_, total_height())）一次性 blit 到屏幕上的 (x_, y_) 位置。注意 dst_x 和 dst_y 是 int32_t 类型——当窗口被拖到屏幕左侧或上方时坐标可能为负，Canvas::blit 内部会做裁剪。

hit testing 两个方法都接收屏幕坐标。contains 检查点是否在窗口矩形内（x_ 到 x_+w_, y_ 到 y_+total_height()）。is_close_button_hit 把关闭按钮的位置从窗口局部坐标转换到屏幕坐标，然后做矩形包含测试。

### window_manager.hpp — WindowManager 声明

WindowManager 是一个 Meyers' singleton，通过 instance() 静态方法获取。它管理最多 64 个窗口指针。常量 DESKTOP_COLOR 定义了深青色桌面背景。

核心数据结构是一个 Window* 数组 windows_[MAX_WINDOWS]，用 count_ 追踪有效数量。focused_ 指针直接指向最顶层窗口，避免每次都算 count_ - 1。

拖拽状态用三个成员表示：dragging_ 布尔标志、drag_offset_x_ 和 drag_offset_y_ 偏移量。

光标位图 k_cursor_bitmap 是一个 16 行的 uint16_t 数组，编码了经典箭头形状。MSB 是最左边的像素，1 表示填充，0 表示透明。

### window_manager.cpp — WindowManager 实现

instance() 就是标准的 Meyers' singleton：`static WindowManager wm; return wm;`。零初始化，第一次调用时构造，程序结束前析构。

create 方法先检查容量，然后 new 一个 Window 对象（位置交错），绘制标题栏和内容，更新焦点，返回 ID。交错位置的计算是 `count_ * 30`——每个新窗口相对上一个偏移 30px，这样连续创建的窗口不会完全重叠。

destroy 通过 find_index 定位窗口，然后调 remove_at。remove_at 做三件事：delete 对象、shift 后续指针、清空末尾槽位、减少计数。如果被删的是焦点窗口就重新计算焦点。

raise 是 Z-序管理的核心操作。实现方式是把目标窗口指针从当前位置抽出来，上面的窗口依次往下挪一位，然后放到数组末尾：

```cpp
Window* win = windows_[idx];
for (uint32_t i = idx; i < count_ - 1; i++) {
    windows_[i] = windows_[i + 1];
}
windows_[count_ - 1] = win;
```

这是一个 O(n) 的数组 shift 操作。对于最多 64 个窗口来说性能完全不是问题。如果窗口数量增长到上千个，可以考虑用链表或跳表。

composite 的流程是：clear → blit all windows → draw_cursor → flip。先清屏为深青色桌面背景，然后从低 Z-order 到高依次 blit 各窗口的离屏画布，再画鼠标光标，最后 flip 到硬件 framebuffer。

handle_mouse 实现了拖拽状态机。MouseDown 做 hit test → 关闭按钮检测 → raise → 标题栏检测 → 开始拖拽。MouseMove 在拖拽状态下移动窗口。MouseUp 结束拖拽。

handle_key 在 tag 030 中是一个空操作——`(void)ev;`。键盘事件虽然被入队并在 tick 回调里分发给 handle_key，但当前版本不做任何处理，留待后续 tag 实现。

update_focus 清除所有窗口的 focused 标记，然后给最顶层窗口设上。这是一个 O(n) 操作，但 n 最大 64，无所谓。

## 设计决策

### 决策：指针数组 vs 链表

**问题**: 窗口用什么数据结构管理？
**本项目的做法**: 固定容量 Window* 指针数组。
**备选方案**: 链表，或 std::vector 等动态容器。
**为什么不选备选方案**: 链表不支持 O(1) 随机访问，hit test 和 compositing 需要遍历所有窗口，链表的 cache 友好性也差。动态容器需要堆分配器支持，而我们的内核没有 STL。固定数组简单、cache 友好、没有碎片问题，64 个窗口的上限对于教学 OS 绰绰有余。

### 决策：焦点 = 最顶层窗口

**问题**: 焦点策略用什么？
**本项目的做法**: 焦点自动跟随最顶层窗口（click-to-raise + click-to-focus）。
**备选方案**: 焦点和 Z-序分离（click-to-focus 但不 raise，需要额外操作来 raise）。
**为什么不选备选方案**: click-to-raise + click-to-focus 是最简单直观的行为，几乎所有的简单窗口管理器都这么做。分离焦点和 Z-序增加了复杂度，对于教学 OS 没有太大价值。

## 扩展方向

- 窗口缩放：在标题栏或内容区边角添加 resize 手柄 (难度 ⭐⭐)
- 最小化/最大化：添加窗口状态（normal/minimized/maximized）和任务栏 (难度 ⭐⭐)
- 窗口装饰主题：把颜色常量抽成可配置的主题结构 (难度 ⭐)
- 损坏追踪（damage tracking）：记录哪些区域需要重绘，只 blit 脏区域 (难度 ⭐⭐⭐)
- 窗口动画：raise/destroy 时添加淡入淡出效果 (难度 ⭐⭐⭐)

## 参考资料
- OSDev Wiki: [PS/2 Mouse](https://wiki.osdev.org/PS/2_Mouse) — 事件类型参考
- SerenityOS: [WindowServer](https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer) — 损坏追踪和合成器架构参考
- Linux: [Early X11 Window Managers](https://www.x.org/wiki/) — 客户端-服务器窗口管理对比
