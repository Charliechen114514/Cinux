# 030 鼠标光标偏移排查

## 现象

QEMU VNC 中显示两个光标：QEMU 自带的圆点 + 我们绘制的箭头。两者之间存在固定偏移，且方向随初始位置设置变化：
- 初始化到屏幕中央 `(512, 384)` → 我们的箭头在 QEMU 圆点的右下方
- 初始化到 `(0, 0)` → 我们的箭头在 QEMU 圆点的左上方
- 偏移量约等于初始位置坐标值，不随鼠标移动改变

## 根因

**PS/2 协议只报告相对位移（dx/dy），不报告绝对位置。**

QEMU 的 VNC 光标覆盖层使用宿主机的绝对坐标渲染。Guest OS（我们）通过累积 PS/2 dx/dy 从初始位置推算光标位置。两者从不同起点出发，累积相同的位移，因此偏移恒等于初始位置的差值。

这不是代码 bug，而是 PS/2 鼠标在 VM 中的已知固有缺陷。

## 尝试过的方案

| 方案 | 结果 |
|------|------|
| 初始化到屏幕中央 | 偏移 ≈ (512, 384)，方向不一致 |
| 初始化到 (0, 0) | 偏移减小但仍然存在，方向反转 |
| 修改 bitmap 像素值 | 只影响箭头形状，不影响位置 |
| `-display default,show-cursor=off` | QEMU 只编译了 `none` 后端，该选项无效 |
| `-display gtk,show-cursor=off` | 用户环境无 GTK |
| `-usb -device usb-tablet` | **有效** — VNC 光标使用绝对定位，鼠标不再卡住 |

## 最终方案

1. **QEMU 配置**：添加 `-usb -device usb-tablet`，VNC 光标使用绝对坐标，解决鼠标抓取卡住问题
2. **Guest 初始位置**：`mouse_x_ = 0, mouse_y_ = 0`，用户从左上角手动移入对齐
3. **长期方案**：实现 USB HID 驱动，读取 USB tablet 的绝对坐标，彻底消除双光标偏移

## 技术背景

### 为什么 PS/2 无法与宿主光标对齐

```
宿主机: cursor = (absolute_x, absolute_y)  ← VNC 客户端直接知道
Guest:  cursor = (init_x + Σdx, init_y + Σdy)  ← 只能累积位移
```

两者之间没有同步机制。PS/2 协议（1980s 设计）没有 "get absolute position" 命令。

### 为什么 `-device usb-tablet` 有效

USB HID Tablet 设备通过 HID Absolute Pointer 用途报告绝对坐标 (0~width, 0~height)。QEMU 将宿主鼠标的绝对位置直接映射到 tablet 坐标，Guest OS 如果有 USB HID 驱动就能读到精确位置。

当前我们没有 USB 驱动，但 QEMU 在宿主侧处理 tablet，VNC 光标仍然用绝对定位渲染，解决了鼠标抓取问题。两个光标仍然独立，但不影响可用性。

### 参考

- [QEMU Mouse Cursor Offset](https://torgeir.dev/2024/02/qemu-mouse-cursor-offset/)
- [ArchWiki: QEMU/Troubleshooting](https://wiki.archlinux.org/title/QEMU/Troubleshooting)
- [QEMU GitLab #2225: Mouse capture issue](https://gitlab.com/qemu-project/qemu/-/issues/2225)
