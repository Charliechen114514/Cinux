# M2: ARP 地址解析

> ARP 协议：IP 地址 → MAC 地址映射。IPv4 通信的前置。

## 任务清单

### T1: ARP 表

```cpp
struct ArpEntry {
    uint32_t ip;
    uint8_t  mac[6];
    uint64_t timestamp;  // 超时淘汰
};

class ArpTable {
public:
    bool lookup(uint32_t ip, uint8_t out_mac[6]);
    void add(uint32_t ip, const uint8_t mac[6]);
    void remove(uint32_t ip);
    void age();  // 清理超时条目（>60s）
};
```

- [ ] ARP 缓存哈希表（64 桶）
- [ ] lookup/add/remove
- [ ] 超时淘汰

### T2: ARP 协议

- [ ] ARP 请求发送（广播）
- [ ] ARP 回复处理（更新缓存）
- [ ] 请求等待队列（IP 未解析时阻塞发送者）
- [ ] gratuitous ARP（主动宣告自己）
