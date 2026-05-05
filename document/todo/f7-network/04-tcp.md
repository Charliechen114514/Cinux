# M5: TCP 协议

> 可靠连接传输层。三次握手、四次挥手、拥塞控制。
> 复杂度最高的网络层。

## 任务清单

### T1: TCP 状态机

```
CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
                                              → CLOSE_WAIT → LAST_ACK → CLOSED
LISTEN → SYN_RCVD → ESTABLISHED
```

- [ ] TcpState enum + 状态转换表
- [ ] TCP 控制块 (TCB) 结构体

### T2: TCP 段处理

```cpp
struct TCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_flags;  // SYN/ACK/FIN/RST
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_pointer;
} __attribute__((packed));
```

- [ ] SYN 处理（新建连接）
- [ ] ACK 处理（确认收到的数据）
- [ ] FIN 处理（关闭连接）
- [ ] RST 处理（异常终止）
- [ ] 数据段收发

### T3: 滑动窗口

- [ ] 发送窗口管理
- [ ] 接收窗口通告
- [ ] 超时重传（RTO 计算）
- [ ] 序列号正确性检查

### T4: 拥塞控制（简化版）

- [ ] 慢启动（cwnd 从 1 MSS 开始指数增长）
- [ ] 拥塞避免（线性增长）
- [ ] 快速重传（3 个重复 ACK）
- [ ] 不做快速恢复（简化）

### T5: TCP Socket

- [ ] listen() — 标记为被动打开
- [ ] accept() — 从完成队列取连接
- [ ] connect() — 主动三次握手
- [ ] send() / recv() — 流式数据传输
- [ ] close() — 四次挥手
