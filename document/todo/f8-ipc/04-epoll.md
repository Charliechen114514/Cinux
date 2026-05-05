# M5: epoll 事件通知

> Linux epoll 机制：epoll_create/epoll_ctl/epoll_wait。
> 高效 I/O 多路复用，事件驱动程序必需。

## 目标

实现 epoll（event poll），替代 select/poll 的高性能 I/O 多路复用机制。
musl/glibc 内部大量使用 epoll。

## 任务清单

### T1: epoll 数据结构

**文件**: `kernel/fs/epoll.hpp`, `kernel/fs/epoll.cpp`

```cpp
struct EpollItem {
    File*     file;
    uint32_t  events;     // 关注的事件（EPOLLIN/EPOLLOUT/EPOLLERR）
    uint32_t  revents;    // 就绪事件
    EpollItem* next;
};

class EpollInstance {
public:
    // 添加/修改/删除监听的 fd
    int ctl(int op, int fd, uint32_t events, void* data);

    // 等待事件就绪
    int wait(struct epoll_event* events, int maxevents, int timeout_ms);

private:
    EpollItem* items_[EPOLL_MAX_FDS];  // fd → item 映射
    EpollItem* ready_list_;             // 就绪链表
    Spinlock   lock_;
    ConditionVariable cv_;
};
```

- [ ] EpollInstance 类
- [ ] fd → EpollItem 映射（256 条目）

### T2: Syscall

| Syscall | 编号 | 说明 |
|---------|------|------|
| epoll_create1 | 291 | 创建 epoll 实例（返回 fd） |
| epoll_ctl | 233 | 添加/修改/删除监听 |
| epoll_wait | 232 | 等待就绪事件 |

```cpp
struct epoll_event {
    uint32_t events;  // EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP
    union {
        void* ptr;
        int   fd;
    } data;
};
```

- [ ] sys_epoll_create1 — 创建 EpollInstance + 分配 fd
- [ ] sys_epoll_ctl — EPOLL_CTL_ADD/MOD/DEL
- [ ] sys_epoll_wait — 遍历就绪链表 + 超时等待
- [ ] epoll fd 关闭时清理

### T3: 文件就绪通知

- [ ] Pipe：有数据可读 → EPOLLIN，缓冲区不满 → EPOLLOUT
- [ ] TTY：输入可用 → EPOLLIN
- [ ] Socket：连接就绪/数据到达 → EPOLLIN，可发送 → EPOLLOUT
- [ ] 文件：始终就绪（磁盘 I/O 同步）

### T4: 集成到 FDTable

- [ ] epoll fd 作为特殊文件描述符（InodeOps: read/write 返回 EPERM）
- [ ] close(epoll_fd) → 清理所有监听项
- [ ] 被监听 fd 关闭时自动从 epoll 移除

### T5: 单元测试

- [ ] epoll_create + epoll_ctl_add + epoll_wait
- [ ] pipe fd 监听：写入后 wait 返回 EPOLLIN
- [ ] 超时：无事件时正确返回 0
- [ ] EPOLL_CTL_DEL 移除后不再触发

## 产出物

- [ ] `kernel/fs/epoll.hpp` / `.cpp`
- [ ] epoll_create1/epoll_ctl/epoll_wait syscall
- [ ] Pipe/TTY/Socket 就绪通知集成
- [ ] libc epoll wrapper
