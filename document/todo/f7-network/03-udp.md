# M4: UDP 协议

> 无连接传输层。简单高效，DHCP/DNS 等应用的基础。

## 任务清单

### T1: UDP 协议

```cpp
struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));
```

- [ ] UDP 发送：构建头部 + payload → IPv4 发送
- [ ] UDP 接收：校验和验证 + 端口分发到 socket
- [ ] 端口管理（绑定/查找）

### T2: UDP Socket（基础）

- [ ] socket(AF_INET, SOCK_DGRAM) 创建
- [ ] bind() 绑定端口
- [ ] sendto() / recvfrom() 收发
- [ ] 简单端口表（256 个端口槽位）
