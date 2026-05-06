# M6: Socket API Syscall

> BSD Socket 系统调用。用户态程序的网络编程接口。

## 任务清单

### T1: Socket 抽象层

```cpp
class Socket {
public:
    virtual ~Socket() = default;
    virtual int bind(const struct sockaddr* addr, size_t len) = 0;
    virtual int listen(int backlog) = 0;
    virtual Socket* accept(struct sockaddr* addr, size_t* len) = 0;
    virtual int connect(const struct sockaddr* addr, size_t len) = 0;
    virtual int64_t send(const void* buf, size_t len, int flags) = 0;
    virtual int64_t recv(void* buf, size_t len, int flags) = 0;
    virtual int close() = 0;

    int domain_;   // AF_INET
    int type_;     // SOCK_STREAM / SOCK_DGRAM
    int protocol_;
};
```

- [ ] Socket 基类
- [ ] TcpSocket 继承 Socket
- [ ] UdpSocket 继承 Socket
- [ ] UnixSocket 继承 Socket（AF_UNIX，F8 实现）

### T2: Syscall 注册

| Syscall | 编号 | 说明 |
|---------|------|------|
| sys_socket | 41 | 创建 socket |
| sys_bind | 49 | 绑定地址 |
| sys_listen | 50 | 监听连接 |
| sys_accept | 43 | 接受连接 |
| sys_connect | 42 | 发起连接 |
| sys_sendto | 44 | 发送数据 |
| sys_recvfrom | 45 | 接收数据 |
| sys_setsockopt | 54 | 设置选项 |
| sys_getsockopt | 55 | 获取选项 |
| sys_shutdown | 48 | 关闭连接 |

- [ ] 所有 socket syscall 注册
- [ ] Socket fd 与 FDTable 集成
- [ ] libc socket wrapper

### T3: 测试

- [ ] TCP echo server/client
- [ ] UDP 收发测试
- [ ] QEMU 网络通信验证
