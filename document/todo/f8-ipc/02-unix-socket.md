# M3: Unix Domain Socket

> 本地 IPC socket (AF_UNIX)。与 F7 Socket 共享接口，不走网络协议栈。

## 任务清单

### T1: UnixSocket 实现

**文件**: `kernel/ipc/unix_socket.hpp`, `kernel/ipc/unix_socket.cpp`

```cpp
class UnixSocket : public Socket {
public:
    int bind(const sockaddr* addr, size_t len) override;
    int listen(int backlog) override;
    Socket* accept(sockaddr* addr, size_t* len) override;
    int connect(const sockaddr* addr, size_t len) override;
    int64_t send(const void* buf, size_t len, int flags) override;
    int64_t recv(void* buf, size_t len, int flags) override;

private:
    enum class State { Unconnected, Listening, Connected };
    State state_;
    ByteRingBuffer<8192> recv_buf_;  // 接收缓冲区
    UnixSocket* peer_;               // 连接的对端
    Mutex lock_;
    ConditionVariable recv_cv_;
    ConditionVariable send_cv_;
};
```

- [ ] socket(AF_UNIX, SOCK_STREAM) 创建
- [ ] bind() 绑定到文件系统路径
- [ ] listen() / accept() 服务器端
- [ ] connect() 客户端连接
- [ ] send/recv 通过环形缓冲区传递数据
- [ ] 等待队列阻塞（非轮询）

### T2: SOCK_DGRAM 支持

- [ ] Unix DGRAM socket（无连接，类似 UDP 但本地）
- [ ] sendto / recvfrom 按地址发送

## 产出物

- [ ] `kernel/ipc/unix_socket.hpp` / `.cpp`
- [ ] 集成到 Socket 层
