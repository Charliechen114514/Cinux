---
title: 032-gui-bitmap-icon-3 · 位图与图标
---

# 032-3 Read-through: WindowManager 集成与测试

## 概览

本篇是 tag 032_gui_bitmap_icon 的第三篇也是最后一篇 read-through，聚焦于两个主题：WindowManager 如何集成桌面图标支持，以及覆盖整个 tag 功能的双轨测试体系。前两篇我们已经看了 Canvas 的 draw_bitmap 实现和编译期图标数据系统，这一篇把它们组装到一起——WindowManager 在 composite() 时渲染图标、在 handle_mouse() 时响应图标点击、测试系统验证像素渲染和命中测试的正确性。

关键设计决策一览：图标渲染在窗口之前（图标被窗口覆盖）、图标命中测试在窗口之前（但优先级低于窗口标题栏）、使用固定大小数组存储图标（最大 16 个）、pending action 机制解耦检测和执行。

## 架构图

```
WindowManager
   │
   ├─ icons_[MAX_ICONS]     (DesktopIcon 数组)
   ├─ icon_count_           (已注册图标数)
   ├─ pending_icon_action_  (待处理动作)
   │
   ├─ add_desktop_icon(icon)    → 注册图标
   ├─ hit_test_icon(mx, my)     → 反向遍历，返回命中的图标
   ├─ consume_pending_icon_action() → 消费动作
   ├─ draw_desktop_icons(screen) → 渲染所有图标
   │
   └─ composite() 流程:
        clear(DESKTOP_COLOR)
        → draw_desktop_icons(screen)    ← 图标层
        → blit 每个窗口 (Z-order)      ← 窗口层
        → draw_cursor(screen)
        → flip()
```

## 代码精讲

### WindowManager 新增的图标相关成员

在 `kernel/gui/window_manager.hpp` 中，WindowManager 类新增了常量和成员来支持桌面图标。

常量定义：

```cpp
static constexpr uint32_t MAX_ICONS        = 16;
static constexpr uint32_t ICON_LABEL_COLOR = 0x00FFFFFF;  // White icon labels
```

`MAX_ICONS = 16` 是一个经验值——一个桌面环境不太可能同时显示超过 16 个图标。这个限制用固定大小数组而不是动态容器来实现，因为我们不希望窗口管理器的图标管理依赖堆分配（虽然内核有堆，但减少运行时分配是好的实践）。

新增的成员变量：

```cpp
DesktopIcon icons_[MAX_ICONS]    = {};
uint32_t    icon_count_          = 0;
IconAction  pending_icon_action_ = IconAction::None;
```

`icons_` 数组用值初始化（`= {}`），所有 DesktopIcon 的指针和数值都被初始化为零。`icon_count_` 追踪已注册的图标数量。`pending_icon_action_` 是一个"一次性信箱"——当用户点击图标时，handle_mouse 把动作写入这个字段；外部调用者（比如桌面事件循环）通过 consume_pending_icon_action() 读取并清空它。这种设计把"检测到点击"和"执行动作"解耦开来，让 handle_mouse 不需要知道动作的具体执行逻辑。

### add_desktop_icon — 图标注册

方法逻辑很直接：如果图标数组未满，把传入的 DesktopIcon 复制到数组末尾，递增计数器，返回 true；如果满了返回 false。

这里有一个细节值得思考：为什么不做图标位置的去重或重叠检测？答案是"不需要"——DesktopIcon 是一个纯数据容器，WindowManager 只负责渲染和命中测试。如果开发者注册了两个完全重叠的图标，系统会忠实地在同一个位置画两个图标，并且后注册的那个在命中测试时优先（因为 hit_test_icon 反向遍历）。这种"不做过多假设"的设计给上层调用者最大的灵活性。

### hit_test_icon — 命中测试

```cpp
const DesktopIcon* hit_test_icon(int32_t mx, int32_t my) const;
```

方法从 icon_count_ - 1 开始向 0 反向遍历，对每个图标调用 contains(mx, my)。一旦找到包含该坐标的图标就立即返回指针。如果没有任何图标命中则返回 nullptr。

反向遍历的设计和窗口的 hit_test 逻辑一致——后注册的元素优先。这意味着如果两个图标重叠，用户点击重叠区域时会触发后注册的那个图标的动作。在实际使用中，图标不应该重叠（桌面环境的布局算法会确保这一点），但命中测试的行为是确定性的。

### consume_pending_icon_action — 消费动作

```cpp
IconAction consume_pending_icon_action() {
    IconAction action = pending_icon_action_;
    pending_icon_action_ = IconAction::None;
    return action;
}
```

一个简单的"读取并清空"操作。先保存当前值，重置为 None，返回保存的值。这个方法是一次性的——调用两次，第二次一定返回 None。

### draw_desktop_icons — 渲染所有图标

这个方法在 composite() 中被调用，负责把所有注册的图标画到屏幕上。对每个图标，它做两件事：调用 `draw_bitmap()` 画图标图案，然后计算标签文字的居中位置并调用 `draw_text()` 画标签。

标签居中的计算逻辑：标签宽度等于字符数乘以字体字符宽度，标签起始 x 坐标等于图标中心 x 减去标签宽度的一半。图标中心 x 是 `icon.x + icon.width / 2`。这保证了无论标签多长（只要不超出屏幕），文字都显示在图标正下方居中位置。

### 测试体系 — 内核端

内核端测试在 `kernel/test/test_bitmap_icon.cpp` 中，约 516 行。测试入口函数 `run_bitmap_icon_tests()` 注册在 main_test.cpp 中，在内核启动后被调用。

测试的 setup 部分创建一个真实的 Framebuffer 对象并从 BootInfo 初始化（物理地址 0x7000）。每个测试创建自己的 Canvas 实例，执行操作后调用 flip()，然后通过 `g_fb.get_pixel()` 直接读取硬件帧缓冲区来验证像素值。

测试用例按功能分组：

**渲染测试组**（test_bitmap_render_opaque, test_bitmap_render_at_origin, test_bitmap_render_larger）：验证 draw_bitmap 能正确地把像素写到指定位置。比如 test_bitmap_render_opaque 画一个 2x2 的四色位图，然后检查四个位置的像素值分别等于红、绿、蓝、白。

**透明测试组**（test_bitmap_transparent_skip, test_bitmap_all_transparent）：先填充一个非零背景色，然后画一个部分透明的位图，验证透明位置的像素保留了背景色、不透明位置的像素被正确覆盖。

**裁剪测试组**（test_bitmap_clip_right, test_bitmap_clip_bottom, test_bitmap_outside_canvas, test_bitmap_null_pixels）：验证位图在 Canvas 边缘和完全超出时的行为。test_bitmap_clip_right 把一个 4 像素宽的位图放在 Canvas 右边缘只剩 2 像素的位置，验证只有 2 个像素被写入。

**32x32 图标测试组**（test_bitmap_render_test_icon, test_bitmap_render_test_icon2, test_bitmap_render_two_icons）：测试辅助函数 build_test_icon 和 build_test_icon2 生成的测试图标，验证透明角、边框、交叉线、显示区域等特征像素的颜色值。

**命中测试组**（test_desktop_icon_contains_inside, test_desktop_icon_contains_outside, test_desktop_icon_contains_negative_position 等）：验证 DesktopIcon 的 contains 方法在各种边界条件下的行为，包括正负坐标、1x1 最小图标、IconAction 值不影响命中测试等。

### 测试体系 — 宿主端

宿主端测试在 `test/unit/test_bitmap_icon.cpp` 中，约 545 行。它定义了一个 MockCanvas 类，用 `std::vector<uint32_t>` 模拟后端缓冲区。draw_bitmap 的实现和内核端完全一致（逐字面复制），因此可以独立验证渲染逻辑。

MockCanvas 和真实 Canvas 的区别在于：没有 Framebuffer 依赖、没有 pitch 的复杂性（pitch = width * 4）、通过 `back_pixel(x, y)` 方法读取后端缓冲区（真实 Canvas 需要通过 Framebuffer 的 get_pixel）。这使得宿主端测试的 setup 更简单、运行更快。

宿主端测试额外覆盖了一些内核端没有的边界用例：棋盘格模式（checkerboard，透明和不透明交替）、零宽零高位图（应该是 no-op）、1x1 位图在 Canvas 精确边界、多层叠加后透明像素保留底层内容等。

## 设计决策

### 决策：图标渲染层级——在窗口之前还是之后

**问题**: 桌面图标应该渲染在窗口的上面还是下面？

**本项目的做法**: 在窗口之前渲染（图标被窗口覆盖）。composite() 的顺序是：clear → draw_desktop_icons → blit windows → draw_cursor → flip。

**备选方案**: 在窗口之后渲染（图标覆盖窗口），或者对每个窗口计算遮挡区域，只在窗口不遮挡的位置画图标。

**为什么不选备选方案**: 图标覆盖窗口的视觉体验很奇怪——用户期望图标是桌面的一部分，窗口可以遮住桌面。精确遮挡计算虽然在视觉上最优，但实现复杂度高，需要逐像素判断遮挡关系。当前方案简单直观，符合桌面环境的常规层级模型。

## 扩展方向

1. **双击检测** — 区分单击和双击，单击选中图标，双击打开
2. **图标选中状态** — 点击后图标高亮（反色或者添加蓝色边框）
3. **图标拖拽** — 支持拖动桌面图标改变位置
4. **右键菜单** — 右键点击图标弹出上下文菜单

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/VBE) — 双缓冲渲染
- SerenityOS: [Painter.cpp](https://github.com/SerenityOS/serenity/blob/master/Userland/Libraries/LibGfx/Painter.cpp) — `blit_filtered()` 中 alpha==0 跳过逻辑
