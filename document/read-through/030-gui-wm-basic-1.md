# 030-1 Read-through: PS/2 Mouse Driver + Event Queue

## 概览

本文 walkthrough 覆盖 tag 030 中 PS/2 鼠标驱动和 GUI 事件系统的完整代码。这两个模块是 GUI 输入子系统的底层——鼠标驱动负责从硬件读取原始数据并解析成结构化事件，事件队列则提供了一个统一的中转站，让中断处理函数和窗口管理器解耦。在整个 tag 中，它们是最先被编写和测试的部分。

关键设计决策：驱动采用全静态方法设计（单例隐含），事件队列是 lock-free ring buffer，容量 128 条。

## 架构图

```
                    +-------------+
                    | PS/2 Mouse  |
                    | (Hardware)  |
                    +------+------+
                           |
                    IRQ12  |  byte stream
                           v
                +----------+-----------+
                | Mouse::irq12_handler |
                |  (ISR context)       |
                +----------+-----------+
                           |
                  process_byte() / decode_packet()
                           |
                           v
                +----------+-----------+
                | Mouse::g_event_queue_|
                | (EventQueue, 128)    |
                +----------+-----------+
                           |
         +-----------------+-----------------+
         |                                   |
  Keyboard::irq1_handler               PIT tick callback
  (dual dispatch, GUI mode)           (gui_tick_callback)
         |                                   |
         +--------> EventQueue <-------------+
                           |
                      dequeue()
                           v
                 WindowManager::handle_mouse()
                 WindowManager::handle_key()
```

## 代码精讲

### event.hpp — GUI 事件类型与数据结构

事件系统定义了三个核心类型。EventType 是一个 scoped enum，区分五种事件类型：

```cpp
enum class EventType : uint8_t {
    MouseMove = 0,
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp,
};
```

MouseEvent 携带完整的鼠标状态——绝对坐标、相对位移、按钮位掩码和三个独立的 bool 便利字段：

```cpp
struct MouseEvent {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    bool    left;
    bool    right;
    bool    middle;
};
```

KeyEvent 复用了与驱动层 KeyEvent 相同的布局，但放在 gui 命名空间下以解耦。Event 是一个 tagged union，type_ 决定 union 的哪个成员有效。

EventQueue 是一个经典的 ring buffer，容量 128。head_ 是读指针，tail_ 是写指针。空队列时 head == tail；满队列时 (tail + 1) % BUF_SIZE == head——这意味着实际可用容量是 127 条。

### event.cpp — Ring Buffer 实现

enqueue 的逻辑很直白——算出下一个 tail 位置，如果等于 head 说明满了，静默丢弃。否则写到 tail 位置然后推进 tail。在 IRQ 上下文里我们不能阻塞，也不能告警，丢事件是最安全的策略。

```cpp
void EventQueue::enqueue(const Event& ev) {
    uint32_t next = (tail_ + 1) % BUF_SIZE;
    if (next == head_) {
        return;
    }
    buf_[tail_] = ev;
    tail_       = next;
}
```

dequeue 同样简洁——空就返回 false，否则读出 head 位置的元素然后推进 head。clear 直接把 head 和 tail 都归零，等价于逻辑清空。

这里没有任何锁或原子操作。设计上的约束是：生产者（IRQ handler）永远不会被抢占，消费者在关中断保护下操作。对于单核内核来说这足够了。

### mouse.hpp — 鼠标驱动接口

Mouse 类全部是静态方法和静态成员。之所以不用 singleton 是因为鼠标驱动没有需要动态初始化的状态——所有成员都在编译时确定了初始值。对外接口包括 init（8042 初始化）、irq12_handler（中断处理）、poll（从队列取鼠标事件）、x/y（当前坐标）、set_screen_bounds（屏幕尺寸）、event_queue（获取全局队列引用）。

内部方法 process_byte 逐字节积累包缓冲区，decode_packet 解析完整包。静态成员包括 3 字节包缓冲区、包索引、光标坐标、屏幕边界、当前和上一次按钮状态、以及全局 EventQueue 实例。

### mouse.cpp — 驱动实现

PS/2 端口和命令常量被组织在几个命名空间里——Ps2Port（0x60/0x64）、Ps2Cmd（0x20/0x60/0xA8/0xD4）、Ps2Status（INPUT_FULL/OUTPUT_FULL）、MouseCmd（0xF4/0xFA）、Packet0（各个 bit mask）。这比用裸数字可读性好得多。

init 函数是标准的 8042 辅助端口初始化序列。先发 0xA8 启用辅助端口，然后读配置字节、设 bit 1、写回，最后通过 0xD4 前缀发送 0xF4 给鼠标开启 streaming。每一步都有 wait_input_empty / wait_output_full 保护，超时上限 100000 次循环（大约几百毫秒）。

irq12_handler 非常轻量——从 0x60 读一个字节，调 process_byte，发 EOI。process_byte 负责 3 字节包的积累。它的同步机制是检查 byte 0 的 bit 3（ALWAYS_1 标记）：如果索引为 0 但当前字节 bit 3 不是 1，直接丢弃等待下一个合法的包开头。这是一种简单但有效的丢同步恢复策略。

decode_packet 是数据提取的核心。9 位有符号 delta 的提取方式是：先把第二/三字节 cast 成 int32_t，如果第一字节的符号位置位就减去 256。这比 OSDev 推荐的 `(d - ((state << 4) & 0x100))` 写法等价但更直观：

```cpp
int32_t dx = static_cast<int32_t>(b1);
if (b0 & Packet0::X_SIGN) {
    dx -= 256;
}
```

Y 轴处理注意取反——`mouse_y_ -= dy`——因为 PS/2 的正 dy 是物理上移而屏幕 Y 轴向下。之后做边界裁剪，然后提取按钮状态并做边缘检测。pressed 掩码是 `new_buttons & ~prev_buttons_`（新置位的位），released 是 `prev_buttons_ & ~new_buttons`（新清零的位）。

最后根据状态变化生成事件——有位移就推 MouseMove，新按下推 MouseDown，新释放推 MouseUp。一个包可能同时触发多种事件（边移动边按下按钮）。

## 设计决策

### 决策：全静态类 vs Singleton

**问题**: 鼠标驱动应该用 singleton 还是全静态方法？
**本项目的做法**: 全静态方法 + 静态成员变量。
**备选方案**: Singleton（static local reference）。
**为什么不选备选方案**: Mouse 不需要延迟构造，不需要多态，不需要在运行时替换。全静态更简单，零开销。唯一缺点是无法用 RAII 管理资源，但 PS/2 驱动也不需要析构。
**如果要扩展**: 如果未来需要支持多种输入设备（USB mouse, touchpad），应该抽象出一个 InputDevice 基类，Mouse 作为子类。那时需要改用实例化设计。

### 决策：事件队列容量 128

**问题**: Ring buffer 应该多大？
**本项目的做法**: 128 条事件。
**备选方案**: 32, 256, 512。
**为什么不选备选方案**: 32 太小——100Hz PIT 意味着每 tick 可能积累多个事件，突发输入很容易溢出。256+ 没必要——每个 Event 大约 40 字节，128 条占 5KB，已经远超正常需要。128 是 2 的幂次，取模运算可以被编译器优化成位与。

## 扩展方向

- 添加 scroll wheel 支持：用 Intellimouse 扩展的 magic sample rate 序列（200-100-80）启用 4 字节包，第 4 字节是 Z 轴位移 (难度 ⭐)
- 添加 5-button 支持：在 Z 轴基础上再发 200-200-80 序列，第 4 字节低 4 位变成 Z 轴+按钮 (难度 ⭐)
- 实现多事件队列：为每个窗口分配独立队列，避免全局队列成为瓶颈 (难度 ⭐⭐)
- 事件过滤：添加事件过滤器机制，让窗口可以声明只接收特定类型的事件 (难度 ⭐⭐)
- 压力测试：写一个测试，高速发送大量事件验证队列不丢帧 (难度 ⭐)

## 参考资料
- OSDev Wiki: [PS/2 Mouse](https://wiki.osdev.org/PS/2_Mouse) — 3字节包格式, 初始化序列, 9位delta提取
- OSDev Wiki: [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) — 8042命令集, 辅助端口编程
- Intel SDM: Vol.3A Section 6.12 — 中断栈帧布局
