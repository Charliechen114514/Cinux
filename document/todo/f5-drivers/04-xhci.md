# M5: xHCI 最小化（仅 HID）

> xHCI USB 控制器最小化实现，仅支持键盘和鼠标。
> 不做大容量存储、集线器等复杂功能。

## 目标

通过 xHCI 驱动支持 USB 键盘/鼠标，替代 PS/2 接口。
保留 PS/2 作为 fallback。

## 任务清单

### T1: xHCI 控制器初始化

**文件**: `kernel/drivers/usb/xhci.hpp`, `kernel/drivers/usb/xhci.cpp`

```cpp
namespace cinux::drivers::usb {

class XHCIController {
public:
    void init(const pci::PCIDevice& pci_dev);

private:
    volatile struct XHCIRegs* regs_;
    DmaBuffer dcbaa_;       // Device Context Base Address Array
    DmaBuffer cmd_ring_;    // Command Ring
    DmaBuffer event_ring_;  // Event Ring
    uint32_t slot_count_;
};
```

**xHCI 寄存器**:
| 偏移 | 名称 | 用途 |
|------|------|------|
| 0x00 | CAPLENGTH | 能力寄存器长度 |
| 0x04 | HCIVERSION | 接口版本 |
| 0x05 | HCSPARAMS1 | 最大 slot/port 数 |
| 0x0C | HCCPARAMS1 | 64-bit/上下文大小 |
| 0x10 | DBOFF | Doorbell 偏移 |
| 0x14 | RTSOFF | Runtime 寄存器偏移 |
| 0x20+ | Operational | USBCMD/USBSTS/PAGESIZE 等 |

**初始化流程**:
1. PCI 发现（class=0x0C, subclass=0x03, prog_if=0x30）
2. 映射 MMIO 寄存器
3. BIOS handoff（夺取控制器所有权）
4. Reset 控制器（USBCMD.RST）
5. 分配 DCBAA、Command Ring、Event Ring
6. 设置 CRCR/CRCR 寄存器
7. 使能控制器（USBCMD.RS = 1）

- [ ] PCI 发现 + MMIO 映射
- [ ] 控制器 Reset + 使能
- [ ] DCBAA 分配
- [ ] Command Ring + Event Ring 初始化
- [ ] BIOS handoff

### T2: USB 设备枚举

- [ ] 端口检测（PORTSC.CCS = 1）
- [ ] 端口 Reset（PORTSC.PR = 1）
- [ ] 地址分配（SET_ADDRESS 命令）
- [ ] 设备描述符读取（GET_DESCRIPTOR）
- [ ] 配置描述符读取 + 设备配置

### T3: HID 类驱动

**文件**: `kernel/drivers/usb/hid.hpp`, `kernel/drivers/usb/hid.cpp`

```cpp
class HIDDriver {
public:
    // 初始化 HID 设备（解析 report descriptor）
    void init(XHCIDevice* dev);

    // 中断端点轮询回调
    void on_report(const uint8_t* data, size_t len);

    // 输入事件分发
    // 键盘 → 现有 Keyboard 事件队列
    // 鼠标 → 现有 Mouse 事件队列
};
```

- [ ] HID Report Descriptor 基础解析
- [ ] 键盘：Boot Protocol（6KRO），映射到现有 KeyEvent
- [ ] 鼠标：Boot Protocol（X/Y/buttons），映射到现有 MouseEvent
- [ ] 中断传输（IN endpoint polling）

### T4: 与现有输入系统集成

- [ ] xHCI keyboard → kernel/drivers/keyboard 事件队列
- [ ] xHCI mouse → kernel/drivers/mouse 事件队列
- [ ] PS/2 和 USB 共存（优先使用 USB，PS/2 fallback）

### T5: 单元测试

- [ ] xHCI 控制器初始化（QEMU xHCI 设备）
- [ ] USB 键盘按键检测
- [ ] USB 鼠标移动检测
- [ ] 输入事件正确分发到 GUI/console

## 产出物

- [ ] `kernel/drivers/usb/xhci.hpp` / `.cpp` — xHCI 控制器
- [ ] `kernel/drivers/usb/hid.hpp` / `.cpp` — HID 驱动
- [ ] `kernel/drivers/usb/CMakeLists.txt`
- [ ] QEMU `-device qemu-xhci -device usb-kbd -device usb-mouse` 验证
