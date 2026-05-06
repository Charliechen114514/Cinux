# 033-gui-desktop-1: WindowManager 桌面图标管理代码精讲

## 概览

本文精讲 tag 033 中 WindowManager 新增的桌面图标管理代码。在 tag 032 中我们定义了 DesktopIcon 数据结构和 IconAction 枚举，但还没有把它和 WindowManager 串起来。这个 tag 中 WindowManager 成为了桌面图标的实际管理者——负责注册、渲染、命中测试和点击分派。我们要看的代码集中在 `window_manager.hpp` 和 `window_manager.cpp` 中新增的约 96 行。

关键设计决策一览：固定容量数组存储图标、反向遍历做命中测试（后注册优先）、pending action 一次性消费模式、图标始终渲染在窗口之下。

## 架构图

```
composite() 渲染管线：
┌─────────────────────────────────┐
│ screen_->clear(DESKTOP_COLOR)   │  ← 第1层：桌面背景
├─────────────────────────────────┤
│ draw_desktop_icons(*screen_)    │  ← 第2层：桌面图标 + 标签
├─────────────────────────────────┤
│ for each window: blit           │  ← 第3层：窗口（Z序）
├─────────────────────────────────┤
│ draw_cursor(screen)             │  ← 最顶层：鼠标光标
└─────────────────────────────────┘

handle_mouse() 点击分派：
MouseDown → hit_test(窗口) ?
  ├─ 命中窗口 → raise + 拖拽/关闭
  └─ 未命中窗口 → hit_test_icon ?
       ├─ 命中图标 → pending_icon_action_ = icon.action
       └─ 未命中图标 → 清除聚焦
```

## 代码精讲

### window_manager.hpp 新增声明

WindowManager 的头文件新增了三样东西：常量、公开 API 和私有数据成员。

首先是两个新的编译期常量——图标的容量上限和标签颜色：

```cpp
static constexpr uint32_t MAX_ICONS      = 16;
static constexpr uint32_t ICON_LABEL_COLOR = 0x00FFFFFF;  // White icon labels
```

`MAX_ICONS` 定义了 16 个槽位。这个数字在教学 OS 的语境下绰绰有余——一个桌面通常只有几个到十几个图标。`ICON_LABEL_COLOR` 是白色（0x00FFFFFF），用 0x00 开头是因为我们的像素格式是 0x00RRGGBB，最高字节未使用。

然后是三个公开的桌面图标 API：

```cpp
bool add_desktop_icon(const DesktopIcon& icon);
const DesktopIcon* hit_test_icon(int32_t mx, int32_t my) const;
IconAction consume_pending_icon_action();
```

这三个方法构成了图标管理的完整生命周期：注册 → 查询 → 消费。注意 `hit_test_icon` 是 const 方法——它只做查询不修改状态。`consume_pending_icon_action` 不是 const 的，因为它会修改 `pending_icon_action_`。

私有部分新增了一个渲染方法和三个数据成员：

```cpp
void draw_desktop_icons(cinux::drivers::Canvas& screen);

DesktopIcon icons_[MAX_ICONS] = {};
uint32_t    icon_count_       = 0;
IconAction  pending_icon_action_ = IconAction::None;
```

固定数组的初始化使用 `= {}` 确保所有字段被零初始化。`pending_icon_action_` 初始为 None，表示没有待处理的动作。

### add_desktop_icon 实现

注册逻辑非常直接——容量检查、尾部追加、计数器递增：

```cpp
bool WindowManager::add_desktop_icon(const DesktopIcon& icon) {
    if (icon_count_ >= MAX_ICONS) {
        return false;
    }
    icons_[icon_count_] = icon;
    icon_count_++;
    return true;
}
```

这里做的是值拷贝——DesktopIcon 中的 `bitmap` 指针和 `label` 指针被原样复制。这意味着调用方必须确保指针在图标注册后保持有效。在我们的使用场景中，bitmap 指向编译期生成的 `icons::data::k_shell_icon` 数组，label 指向字符串字面量，两者都存在于整个程序的生命周期中，所以没有悬空指针的风险。

### hit_test_icon 实现

```cpp
const DesktopIcon* WindowManager::hit_test_icon(int32_t mx, int32_t my) const {
    for (uint32_t i = icon_count_; i > 0; i--) {
        uint32_t idx = i - 1;
        if (icons_[idx].contains(mx, my)) {
            return &icons_[idx];
        }
    }
    return nullptr;
}
```

这段遍历值得仔细看。循环从 `icon_count_` 向下到 1，实际索引是 `i - 1`（即从最后一个图标到第一个）。这个反向遍历实现了"后注册的图标优先级高"——当两个图标重叠时，后注册的会先被检测到并返回。`contains()` 方法是 DesktopIcon 的矩形命中测试，在 tag 032 中已经实现。

为什么用 `for (uint32_t i = icon_count_; i > 0; i--)` 而不是 `for (int i = icon_count_ - 1; i >= 0; i--)`？因为后者在 `i` 为 `uint32_t` 时会导致 `i >= 0` 永远为真（无符号数下溢），形成死循环。前者的写法用 `i > 0` 做终止条件，`idx = i - 1` 在循环体内计算实际索引，避免了无符号下溢问题。

### consume_pending_icon_action 实现

```cpp
IconAction WindowManager::consume_pending_icon_action() {
    IconAction action = pending_icon_action_;
    pending_icon_action_ = IconAction::None;
    return action;
}
```

经典的 "swap-and-reset" 模式——先保存当前值，再重置为默认，最后返回保存的值。这个方法保证每个 pending action 只被消费一次。gui_tick_callback 在每个 tick 中调用一次，如果没有待处理的动作就返回 None，什么都不做。

### draw_desktop_icons 实现

```cpp
void WindowManager::draw_desktop_icons(cinux::drivers::Canvas& screen) {
    if (font_ == nullptr) {
        return;
    }
    uint32_t glyph_w = font_->width();

    for (uint32_t i = 0; i < icon_count_; i++) {
        const DesktopIcon& icon = icons_[i];

        screen.draw_bitmap(
            static_cast<uint32_t>(icon.x),
            static_cast<uint32_t>(icon.y),
            icon.width,
            icon.height,
            icon.bitmap);

        uint32_t label_len = 0;
        if (icon.label != nullptr) {
            for (const char* p = icon.label; *p != '\0'; p++) {
                label_len++;
            }
        }

        if (label_len > 0) {
            uint32_t text_w = label_len * glyph_w;
            uint32_t label_x = static_cast<uint32_t>(icon.x)
                + (icon.width - text_w) / 2;
            uint32_t label_y = static_cast<uint32_t>(icon.y) + icon.height + 2;

            screen.draw_text(label_x, label_y, icon.label, ICON_LABEL_COLOR, *font_);
        }
    }
}
```

这个函数先做防御性检查——如果 font 为 null 就直接返回，因为渲染标签需要字体。然后遍历每个图标，做两件事。

第一件事是用 `draw_bitmap` 渲染图标的像素数据。这个方法在 tag 032 中实现，会跳过透明像素（0x00000000）并做边界裁剪。

第二件事是渲染标签文字。先用一个手动循环计算字符串长度（内核态没有 `strlen`），然后计算水平居中位置。居中公式是 `icon.x + (icon.width - text_width) / 2`——图标宽度减去文字宽度，差值除以 2 得到两侧留白。标签的 y 坐标在图标底部加 2 像素间距。文字颜色使用前面定义的 `ICON_LABEL_COLOR`（白色）。

### handle_mouse 中的图标点击分支

原来的 MouseDown 处理是"没有窗口命中就清除聚焦"，现在在这中间插入了图标检测：

```cpp
if (hit == nullptr) {
    const DesktopIcon* icon_hit = hit_test_icon(ev.mouse.x, ev.mouse.y);
    if (icon_hit != nullptr) {
        pending_icon_action_ = icon_hit->action;
        if (focused_ != nullptr) {
            focused_->set_focused(false);
            focused_ = nullptr;
        }
    } else {
        if (focused_ != nullptr) {
            focused_->set_focused(false);
            focused_ = nullptr;
        }
    }
    break;
}
```

这里的关键是点击图标时也会清除窗口聚焦。为什么？因为图标在桌面层（窗口之下），点击图标意味着用户的注意力从窗口转移到了桌面。如果不清除聚焦，之前的窗口仍然高亮显示为"激活"状态，这会误导用户。点击桌面空白区域同理——用户明确在"不操作任何窗口"，所以应该取消聚焦。

### composite 中的渲染顺序

```cpp
void WindowManager::composite() {
    screen_->clear(DESKTOP_COLOR);
    draw_desktop_icons(*screen_);
    for (uint32_t i = 0; i < count_; i++) {
        if (windows_[i]->visible()) {
            // blit window...
        }
    }
    draw_cursor(*screen_);
}
```

`draw_desktop_icons` 被插入在 clear 和窗口 blit 之间。这保证了渲染顺序是 背景色 → 图标 → 窗口 → 光标，图标始终在窗口之下。

### init 中的状态重置

```cpp
void WindowManager::init(cinux::drivers::Canvas* screen, cinux::drivers::PSFFont* font) {
    // ... 原有初始化 ...
    icon_count_ = 0;
    pending_icon_action_ = IconAction::None;
}
```

在 init 中重置图标计数器和 pending action，确保 WindowManager 可以安全地重复初始化。测试代码中依赖这个行为——测试之间调用 init 来清除上一个测试的状态。

## 设计决策

### 决策：固定容量数组 vs 动态容器
**问题**: 如何存储桌面图标？
**本项目的做法**: 固定容量 DesktopIcon 数组（16 个）
**备选方案**: 链表或内核堆上的动态数组
**为什么不选备选方案**: 桌面图标数量极少（通常 <10），固定数组零分配且缓存友好。链表需要逐节点堆分配，增加碎片风险。动态数组需要 realloc 语义，内核态没有标准库支持。
**如果要扩展**: 可以改为 `kstd::vector`-like 的动态数组，支持运行时扩容。但对于教学 OS 来说当前方案已经足够。

### 决策：pending action 一次性消费
**问题**: 如何从 handle_mouse 传递点击事件到 gui_tick_callback？
**本项目的做法**: 单变量 + consume 重置
**备选方案**: 事件队列（ring buffer）
**为什么不选备选方案**: 桌面图标点击频率极低（用户每秒最多点几次），单变量足够。如果用队列，还需要处理队列满、事件丢失等边界情况，增加复杂度。
**如果要扩展**: 如果未来需要处理多个并发点击（比如触摸屏多点触控），需要改为事件队列。

## 扩展方向

1. (⭐) 给桌面图标添加选中高亮效果——点击时反色边框，类似 Windows 桌面图标的蓝色选框
2. (⭐) 支持拖拽排列桌面图标——用户可以拖动图标到新位置，位置持久化到文件系统
3. (⭐⭐) 实现图标网格自动排列——类似桌面的"自动排列"功能，图标按网格对齐
4. (⭐⭐) 支持右键上下文菜单——右键点击图标弹出菜单（打开、删除、属性等）
5. (⭐⭐⭐) 实现图标的双击 vs 单击区分——单击选中、双击启动，需要定时器区分

## 参考资料
- SerenityOS WindowManager: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- ToaruOS Yutani Compositor: https://github.com/klange/toaruos
- OSDev Wiki Drawing: https://wiki.osdev.org/VBE
