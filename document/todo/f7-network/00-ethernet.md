# M1: 以太网帧 + 网络设备管理

> 网络设备抽象层和以太网帧收发。连接 F5 驱动和上层协议。

## 任务清单

### T1: 网络设备管理器

**文件**: `kernel/net/netif.hpp`, `kernel/net/netif.cpp`

```cpp
namespace cinux::net {

class NetInterface {
public:
    void init(INetDevice* dev);

    // 帧收发
    void send_frame(const void* data, size_t len);
    void poll_receive();

    // 配置
    void set_ipv4(uint32_t ip, uint32_t netmask, uint32_t gateway);
    uint32_t ip() const;
    uint32_t netmask() const;
    uint32_t gateway() const;
    void get_mac(uint8_t mac[6]) const;

    // 上层协议注册
    using PacketHandler = void (*)(NetInterface*, const void* data, size_t len);
    void set_handler(uint16_t ethertype, PacketHandler handler);

private:
    INetDevice* dev_;
    uint32_t ip_, netmask_, gateway_;
    uint8_t mac_[6];
    PacketHandler handlers_[ETHERTYPE_MAX];
};

extern NetInterface g_netif;

} // namespace cinux::net
```

- [ ] NetInterface 封装 INetDevice
- [ ] 以太网类型分发（IPv4=0x0800, ARP=0x0806）
- [ ] IP/netmask/gateway 配置
- [ ] 全局单例

### T2: 以太网帧处理

```cpp
struct EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));
```

- [ ] 发送：构建 EthernetHeader + payload → NetInterface::send_frame
- [ ] 接收：解析 ethertype → 分发到注册的 handler
- [ ] 接收轮询（在中断或定时器中调用）
