# M7: VirtIO Net 网卡

> 基于 M2 的 VirtIO 框架实现 VirtIO Net 驱动。
> QEMU 半虚拟化网卡，高性能。

## 依赖

- M2 VirtIO 框架（VirtIODevice + VirtQueue）
- M6 INetDevice 接口

## 任务清单

### T1: VirtIO Net 驱动

**文件**: `kernel/drivers/virtio/virtio_net.hpp`, `kernel/drivers/virtio/virtio_net.cpp`

```cpp
class VirtIONet : public INetDevice {
public:
    void init(const pci::PCIDevice& pci_dev);

    bool send(const void* data, size_t len) override;
    int32_t receive(void* buf, size_t buf_size) override;
    void get_mac(uint8_t mac[6]) override;
    bool link_up() override;

private:
    VirtIODevice vdev_;
    VirtQueue    rx_queue_;     // Queue 0: receive
    VirtQueue    tx_queue_;     // Queue 1: transmit
    uint8_t      mac_[6];
};
```

**VirtIO Net 特性**:
- VIRTIO_NET_F_CSUM — 部分校验和卸载
- VIRTIO_NET_F_MAC — 设备提供 MAC 地址
- VIRTIO_NET_F_STATUS — 链接状态查询

**帧格式**:
```
发送：struct virtio_net_hdr + 以太网帧数据
接收：struct virtio_net_hdr + 以太网帧数据

struct virtio_net_hdr {
    uint8_t  flags;       // 校验和标志
    uint8_t  gso_type;    // GSO 类型
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers; // 仅接收
};
```

### T2: 接收和发送

- [ ] receive 队列初始化：预填充空缓冲区到 RX queue
- [ ] send：构建 virtio_net_hdr + data → submit 到 TX queue
- [ ] receive：从 RX queue 取完成 → 拷贝数据 → 重新填充空缓冲区
- [ ] 中断处理：RX 完成中断 + TX 完成清理

### T3: 单元测试

- [ ] VirtIO Net 初始化 + MAC 读取
- [ ] 帧收发验证（QEMU tap backend）
- [ ] 与 E1000 对比测试

## 产出物

- [ ] `kernel/drivers/virtio/virtio_net.hpp` / `.cpp`
- [ ] QEMU `-netdev tap -device virtio-net-pci,netdev=net0` 验证
