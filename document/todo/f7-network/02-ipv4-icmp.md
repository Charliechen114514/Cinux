# M3: IPv4 + ICMP

> IPv4 基础：分片/重组、路由。ICMP 用于 ping 和错误报告。

## 任务清单

### T1: IPv4 数据报

```cpp
struct IPv4Header {
    uint8_t  version_ihl;   // 0x45
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;      // ICMP=1, TCP=6, UDP=17
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));
```

- [ ] IPv4 头部构建和校验和计算
- [ ] 接收：校验和验证 + 协议分发
- [ ] 发送：自动设置 src_ip + TTL
- [ ] 分片重组（接收端，简单实现）

### T2: ICMP

- [ ] Echo Reply（响应 ping）
- [ ] Echo Request 发送（发起 ping）
- [ ] Destination Unreachable 生成
- [ ] TTL Exceeded 生成

### T3: 路由表

```cpp
struct Route {
    uint32_t network;
    uint32_t mask;
    uint32_t gateway;
    NetInterface* iface;
};
```

- [ ] 静态路由表（最长前缀匹配）
- [ ] 默认路由
- [ ] 直连网络判断
