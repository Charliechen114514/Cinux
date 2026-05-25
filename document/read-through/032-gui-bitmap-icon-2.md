---
title: 032-gui-bitmap-icon-2 · 位图与图标
---

# 032-2 Read-through: 图标数据与 DesktopIcon

## 概览

本篇是 tag 032_gui_bitmap_icon 的第二篇 read-through，聚焦于编译期图标数据生成系统和 DesktopIcon 结构体。上一篇讲了 Canvas 底层的 draw_bitmap 渲染能力，这一篇讲的是"画什么"和"怎么组织图标"——从 icon.hpp 的公共接口，到 icon_data.hpp 的 constexpr 像素生成，再到 desktop_icon.hpp 的数据封装和命中测试。

关键设计决策一览：使用 palette-indexed hex 编码让图标定义可读、用 constexpr 模板在编译时完成"字符串→像素"转换、DesktopIcon 用聚合体设计（无构造函数）、命中测试使用标准的点-矩形包含算法。

## 架构图

```
icon.hpp (公共接口)
   │  ICON_SIZE = 32, ICON_PIXELS = 1024
   │
   └─ #include → icon_data.hpp
                    │
                    ├─ palette 命名空间 (颜色常量)
                    ├─ detail::hex_nibble (hex字符→数字)
                    ├─ detail::palette_lookup (索引→颜色)
                    ├─ detail::build_icon<32> (字符串矩阵→像素数组)
                    ├─ k_shell_palette + k_shell_icon (终端图标)
                    └─ k_calc_palette + k_calc_icon (计算器图标)

desktop_icon.hpp (数据封装)
   │
   ├─ IconAction enum class {None, OpenShell, OpenCalculator}
   └─ DesktopIcon struct {x, y, bitmap, label, width, height, action}
        └─ contains(mx, my) → bool
```

## 代码精讲

### icon.hpp — 公共接口

这个头文件非常简洁，总共不到 30 行有效代码。它定义了 `cinux::gui::icons` 命名空间，暴露两个编译时常量：

```cpp
namespace cinux::gui::icons {

constexpr uint32_t ICON_SIZE = 32;
constexpr uint32_t ICON_PIXELS = ICON_SIZE * ICON_SIZE;

}  // namespace cinux::gui::icons

#include "kernel/gui/data/icon_data.hpp"
```

`ICON_SIZE` 和 `ICON_PIXELS` 的分离看似冗余（PIXELS 永远等于 SIZE 的平方），但这是有意的——代码中经常需要引用"图标总像素数"来分配缓冲区或定义数组大小，用命名常量比到处写 `ICON_SIZE * ICON_SIZE` 清晰得多。

头文件末尾的 `#include "kernel/gui/data/icon_data.hpp"` 把实际的像素数据引入进来。这种"接口头文件 include 实现头文件"的模式在内核代码中不太常见（更常见的做法是让用户自己 include 需要的文件），但在这里是合理的——用户只需要 include icon.hpp 就能获得所有图标相关的定义和数据，不需要知道底层分了几个文件。

### icon_data.hpp — 编译期像素生成

这个文件是整个图标系统的精华，分三个层次：调色板定义、构建工具、实际图标。

首先看调色板命名空间。定义了一组全局颜色常量，所有图标共享：

```cpp
namespace palette {
constexpr uint32_t BLACK       = 0x00000000;  // Transparent
constexpr uint32_t DARK_BLACK  = 0x00101010;
constexpr uint32_t WHITE       = 0x00FFFFFF;
constexpr uint32_t GREEN       = 0x0033CC33;
constexpr uint32_t GREY_DARK   = 0x00404040;
// ... 更多颜色
}  // namespace palette
```

这里有一个重要的约定：`BLACK` 的值是 `0x00000000`，和 draw_bitmap 中的透明色完全一致。这不是巧合——调色板中的"黑色"在图标中代表"这个像素不画"，它通过 build_icon 被映射到 hex 字符 '0'，最终在 draw_bitmap 中被跳过。整个透明链路从调色板定义贯穿到渲染逻辑。

接下来是 `detail` 命名空间中的构建工具。`hex_nibble` 把一个 hex 字符转换成 0-15 的数值：

```cpp
consteval uint32_t hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return static_cast<uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<uint32_t>(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return static_cast<uint32_t>(c - 'A') + 10;
    return 0;
}
```

函数很小，但处理了三种字符范围：数字 0-9、小写 a-f、大写 A-F。非法字符返回 0——这意味着如果图案字符串中有拼写错误（比如写了一个 'g'），它会被静默地映射为调色板的第一种颜色（通常是透明黑色）。这个行为在调试时可能会造成困惑，但作为 consteval 函数它无法抛出异常或打印警告。

核心的 `build_icon` 模板：

```cpp
template <uint32_t Rows>
consteval std::array<uint32_t, 1024> build_icon(const uint32_t (&palette)[16],
                                                const char* const (&rows)[Rows]) {
    static_assert(Rows == 32, "Icon must have exactly 32 rows");

    std::array<uint32_t, 1024> pixels{};
    for (uint32_t r = 0; r < 32; r++) {
        for (uint32_t c = 0; c < 32; c++) {
            uint32_t nibble    = hex_nibble(rows[r][c]);
            pixels[r * 32 + c] = palette_lookup(palette, nibble);
        }
    }
    return pixels;
}
```

模板参数 `Rows` 是数组引用的大小，static_assert 确保它恰好是 32。这种"通过数组引用的大小来推导模板参数"的技巧在 C++ 中很常见——它让编译器自动计算传入了多少行字符串，然后我们在编译时验证行数是否正确。

函数体是一个简单的双重循环：对每个像素位置，取对应位置的 hex 字符，转换成 nibble，从调色板查出颜色值，写入结果数组。整个函数在编译时执行，产出的 pixels 数组存储在二进制文件的 `.rodata` 段。

最后看 Shell 图标的定义。先定义它的专属调色板：

```cpp
inline constexpr uint32_t k_shell_palette[16] = {
    palette::BLACK,       // 0 - transparent
    palette::DARK_BLACK,  // 1 - terminal body
    palette::GREY_DARK,   // 2 - title bar
    palette::WHITE,       // 3 - text
    palette::GREEN,       // 4 - cursor green
    0x00CC3333,           // 5 - close dot red
    0x00CCCC33,           // 6 - minimise dot yellow
    0x0033CC33,           // 7 - maximise dot green
};
```

只有前 8 个位置被使用（0-7），其余位置默认初始化为 0（也就是透明）。这意味着图案字符串中如果出现 '8' 到 'f' 的字符，对应的像素会是透明的。

然后是 32 行 hex 字符串定义图案，通过 build_icon 生成像素数组。图案的视觉效果是一个深色圆角终端窗口，顶部有红黄绿三个小圆点，中间有白色的 ">_" 命令提示符。

Calculator 图标使用不同的调色板和图案，视觉上是一个灰色计算器机身，顶部有绿色 LCD 显示屏，下面是按钮网格。

### desktop_icon.hpp — 数据封装与命中测试

`IconAction` 枚举用 scoped enum 定义：

```cpp
enum class IconAction : uint8_t {
    None           = 0,
    OpenShell      = 1,
    OpenCalculator = 2,
};
```

底层类型是 `uint8_t`——枚举值只用到 0-2，用 uint8_t 足够。scoped enum 防止了隐式转换和命名冲突，这是现代 C++ 的标准做法。

`DesktopIcon` 结构体：

```cpp
struct DesktopIcon {
    int32_t         x;
    int32_t         y;
    const uint32_t* bitmap;
    const char*     label;
    uint32_t        width;
    uint32_t        height;
    IconAction      action;

    [[nodiscard]] bool contains(int32_t mx, int32_t my) const {
        return mx >= x && mx < static_cast<int32_t>(x + width) &&
               my >= y && my < static_cast<int32_t>(y + height);
    }
};
```

x 和 y 是 int32_t——支持负坐标（部分离屏）。bitmap 指针指向编译期生成的像素数组（比如 `k_shell_icon.data()`）。label 是图标的文字标签，显示在图标下方。width 和 height 是 uint32_t——图标尺寸不应该为负。

contains 方法的实现是标准的矩形包含测试。注意 `static_cast<int32_t>(x + width)` 这一步：因为 x 是 int32_t、width 是 uint32_t，加法会先把 x 提升为 uint32_t，然后做无符号加法，最后转回 int32_t。当 x 为正值时这不会出问题；当 x 为负值时（比如 x = -10, width = 32），无符号加法的结果是正确的（得到 22），static_cast 回 int32_t 后也是 22。`[[nodiscard]]` 属性提醒调用者不要忽略返回值——命中测试的结果一定要被使用。

## 设计决策

### 决策：调色板索引编码 vs 直接颜色值

**问题**: 图标像素数据用什么格式编码？

**本项目的做法**: 使用 palette-indexed hex 编码——每像素一个 hex 字符，通过调色板查找表映射为实际颜色。

**备选方案**: 直接存储 `0x00RRGGBB` 值，每个像素用一个 32 位整数。

**为什么不选备选方案**: 直接存储 1024 个 32 位整数会让图标定义完全不可读——无法从数字中看出图标的形状。调色板索引方案让图标定义像 ASCII art，开发者可以直接"看"到图标的视觉形状，修改起来也更方便。

**如果要扩展**: 当前方案限制最多 16 种颜色（一个 hex 字符的范围）。如果需要更多颜色，可以扩展为两个 hex 字符（最多 256 种颜色），但这会牺牲一半的可读性。

## 扩展方向

1. 支持不同尺寸的图标（16x16、48x48、64x64）
2. 添加更多预设图标（文件管理器、设置、浏览器等）
3. 支持从 ramdisk 文件系统加载图标数据
4. 实现图标的多分辨率变体（高 DPI 支持）

## 参考资料

- Linux Kernel: `include/linux/linux_logo.h` — 预编译像素数组方式的启动 logo
- SerenityOS: [Bitmap.h](https://github.com/SerenityOS/serenity/blob/master/Userland/Libraries/LibGfx/Bitmap.h) — 运行时加载 PNG 的位图类，与 Cinux 的编译期方案形成对比
