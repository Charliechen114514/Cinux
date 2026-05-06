# F7: 网络协议栈

> 完整 TCP/IP 协议栈。从以太网帧到 Socket API。
> F5 的 E1000/VirtIO-Net 驱动提供数据链路层。

## 实现决策

| 层 | 协议 |
|----|------|
| 数据链路 | 以太网帧收发（F5 驱动层） |
| 网络 | ARP + IPv4 + ICMP |
| 传输 | UDP + TCP |
| API | BSD Socket (socket/bind/listen/accept/connect/send/recv) |

## Milestone 依赖

```
M1 以太网 + 网络设备管理 ──→ M2 ARP
                                ↓
                           M3 IPv4 + ICMP
                                ↓
                          M4 UDP ──→ M5 TCP
                                      ↓
                                M6 Socket API
```

严格分层：每层依赖下层。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-ethernet.md](00-ethernet.md) | M1: 以太网帧 + 网络设备管理 |
| [01-arp.md](01-arp.md) | M2: ARP 地址解析 |
| [02-ipv4-icmp.md](02-ipv4-icmp.md) | M3: IPv4 + ICMP (ping) |
| [03-udp.md](03-udp.md) | M4: UDP 协议 |
| [04-tcp.md](04-tcp.md) | M5: TCP 协议 |
| [05-socket.md](05-socket.md) | M6: Socket API syscall |

## 验收标准

- ping 外部主机成功
- UDP 收发验证
- TCP echo server/client 测试
- Socket API 与 musl 兼容
