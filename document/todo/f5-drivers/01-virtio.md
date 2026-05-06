# M2: VirtIO 框架 + VirtIO Block

> 实现 VirtIO 传输层框架（PCI 配置 + VirtQueue）。
> 在此基础上实现 VirtIO Block 驱动（IBlockDevice）。

## 目标

VirtIO 是 QEMU/KVM 半虚拟化框架。实现核心框架后，Block 和 Net 驱动共享基础设施。

## 任务清单

### T1: VirtIO PCI 配置

**文件**: `kernel/drivers/virtio/virtio.hpp`, `kernel/drivers/virtio/virtio_pci.cpp`

VirtIO 设备通过 PCI 发现（vendor_id = 0x1AF4）：

```cpp
namespace cinux::drivers::virtio {

enum DeviceID : uint16_t {
    Block    = 0x1001,   // 块设备
    Network  = 0x1000,   // 网卡
    Console  = 0x1003,
    Input    = 0x1052,   // 输入设备
};

class VirtIODevice {
public:
    void init(const pci::PCIDevice& pci_dev);

    // 配置空间访问
    uint32_t read_config(uint32_t offset);
    void write_config(uint32_t offset, uint32_t value);

    // 特性协商
    uint64_t read_device_features();
    void write_driver_features(uint64_t features);
    bool negotiate_features(uint64_t wanted);

    // 状态控制
    void set_status(uint8_t status);
    void reset();

    uint8_t  irq() const;
    DeviceID device_id() const;

protected:
    pci::PCIDevice pci_dev_;
    uint64_t common_cfg_base_;  // MMIO 基地址
    uint64_t notify_base_;
    uint64_t isr_base_;
    uint64_t device_cfg_base_;
};

} // namespace cinux::drivers::virtio
```

**初始化流程**:
1. Reset 设备（status = 0）
2. 设置 ACKNOWLEDGE | DRIVER 位
3. 协商 features
4. 设置 FEATURES_OK 位
5. 分配并注册 VirtQueues
6. 设置 DRIVER_OK 位 → 设备激活

- [ ] VirtIODevice 基类
- [ ] PCI 配置空间读写
- [ ] 特性协商机制
- [ ] 状态机管理

### T2: VirtQueue 实现

**文件**: `kernel/drivers/virtio/virtqueue.hpp`, `kernel/drivers/virtio/virtqueue.cpp`

VirtQueue 是 VirtIO 的核心数据结构（三张表 + 可用/使用环）：

```cpp
struct VirtQueueBuffers {
    struct Desc {
        uint64_t addr;      // 物理地址
        uint32_t len;       // 缓冲区长度
        uint16_t flags;     // VRING_DESC_F_NEXT / WRITE
        uint16_t next;      // 下一个 desc 索引
    };

    struct Available {
        uint16_t flags;
        uint16_t idx;
        uint16_t ring[];
        uint16_t used_event;
    };

    struct UsedElem {
        uint32_t id;
        uint32_t len;
    };

    struct Used {
        uint16_t flags;
        uint16_t idx;
        UsedElem ring[];
        uint16_t avail_event;
    };
};

class VirtQueue {
public:
    void init(uint32_t queue_index, uint16_t queue_size, VirtIODevice* dev);

    // 提交请求
    uint16_t submit(const ScatterGatherEntry* entries, size_t count,
                    bool writable);

    // 等待完成
    bool wait_completion(uint16_t desc_idx, uint32_t* out_len);

    // 中断处理
    void process_used();

    // 通知设备
    void notify();

private:
    VirtQueueBuffers::Desc*  desc_table_;
    VirtQueueBuffers::Available* available_;
    VirtQueueBuffers::Used*  used_;
    uint16_t queue_size_;
    uint16_t last_used_idx_;
    VirtIODevice* dev_;
    uint32_t queue_index_;
    DmaBuffer dma_buf_;   // 整个 VirtQueue 的 DMA 缓冲
};
```

- [ ] 分配 desc/available/used 三张表（DMA Pool，页对齐）
- [ ] submit() — 构建描述符链，更新 available ring
- [ ] notify() — 写 notify 寄存器
- [ ] process_used() — 处理完成事件
- [ ] 等待队列集成（阻塞 + 中断唤醒）

### T3: VirtIO Block 驱动

**文件**: `kernel/drivers/virtio/virtio_block.hpp`, `kernel/drivers/virtio/virtio_block.cpp`

```cpp
class VirtIOBlock : public IBlockDevice {
public:
    void init(const pci::PCIDevice& pci_dev);

    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override { return 512; }

private:
    VirtIODevice vdev_;
    VirtQueue    request_queue_;
    uint64_t     capacity_;  // 块数量
};
```

**VirtIO Block 请求格式**:
```cpp
struct BlockRequest {
    uint32_t type;       // VIRTIO_BLK_T_IN / VIRTIO_BLK_T_OUT / VIRTIO_BLK_T_FLUSH
    uint32_t reserved;
    uint64_t sector;     // 起始扇区
    uint8_t  data[];     // 数据
    uint8_t  status;     // 返回状态（VIRTIO_BLK_S_OK / S_IOERR / S_UNSUPP）
};
```

- [ ] init — PCI 发现 + VirtQueue 初始化
- [ ] read_blocks — 构建请求 → submit → wait → 拷贝结果
- [ ] write_blocks — 类似，方向相反
- [ ] block_count — 从设备配置读取 capacity
- [ ] 注册到内核驱动管理器

### T4: 单元测试

- [ ] VirtQueue submit + process_used 流程
- [ ] VirtIO Block 在 QEMU 中读写验证
- [ ] 通过 IBlockDevice 接口挂载 ext2 测试

## 产出物

- [ ] `kernel/drivers/virtio/virtio.hpp` / `virtio_pci.cpp` — VirtIO 框架
- [ ] `kernel/drivers/virtio/virtqueue.hpp` / `.cpp` — VirtQueue
- [ ] `kernel/drivers/virtio/virtio_block.hpp` / `.cpp`
- [ ] `kernel/drivers/virtio/CMakeLists.txt`
- [ ] QEMU VirtIO Block + ext2 验证
