# M4: Block Device Abstraction

> 极简 IBlockDevice 接口，解耦 ext2 与 AHCI。
> 为 NVMe、VirtIO Block、Page Cache 提供统一接入点。

## 目标

定义最小化的块设备接口，将 ext2 从 AHCI 硬编码中解耦。
不引入请求队列、异步 I/O 等复杂机制（留给后续 Feature 域）。

## 现有代码

- `kernel/drivers/ahci/ahci.hpp` — `AHCI::read(port, lba, count, phys_buf)` / `write()`
- `kernel/fs/ext2.hpp` — `Ext2` 类持有 `AHCI& ahci_` 和 `uint8_t port_index_`
- `kernel/fs/vfs_filesystem.hpp` — `FileSystem` 抽象基类
- M3 产出的 `dma_pool.hpp` / `prdt.hpp`

## 任务清单

### T1: IBlockDevice 接口定义

**文件**: `kernel/drivers/block_device.hpp`（新增）

```cpp
namespace cinux::drivers {

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;

    // 同步块读写
    virtual bool read_blocks(uint64_t block, uint64_t count, void* buf) = 0;
    virtual bool write_blocks(uint64_t block, uint64_t count, const void* buf) = 0;

    // 设备信息
    virtual uint64_t block_count() const = 0;
    virtual uint64_t block_size() const = 0;

    // 刷新（对于有写缓存的设备）
    virtual void flush() {}
};

} // namespace cinux::drivers
```

- [ ] 定义 `IBlockDevice` 抽象类
- [ ] 纯虚：read_blocks / write_blocks / block_count / block_size
- [ ] 虚函数：flush()（默认空操作）
- [ ] buf 参数为虚拟地址（实现内部负责 DMA 映射）

### T2: AHCIBlockDevice 适配器

**文件**: `kernel/drivers/ahci/ahci_block_device.hpp` / `.cpp`（新增）

```cpp
namespace cinux::drivers::ahci {

class AHCIBlockDevice : public IBlockDevice {
public:
    AHCIBlockDevice(AHCI& ahci, uint8_t port_index);

    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override { return 512; }
    void flush() override;

private:
    AHCI& ahci_;
    uint8_t port_index_;
    dma::DmaBuffer dma_buf_;   // M3 产出的 DMA pool
};

} // namespace cinux::drivers::ahci
```

- [ ] 构造时从 `g_dma_pool` 分配 DMA 缓冲区
- [ ] `read_blocks()`：DMA 读取 → memcpy 到 buf
- [ ] `write_blocks()`：memcpy 到 DMA buf → DMA 写入
- [ ] `block_count()` 从 AHCI identify 获取
- [ ] `flush()` 发送 FLUSH CACHE 命令

### T3: RAMBlockDevice（测试用）

**文件**: `kernel/drivers/ram_block_device.hpp`（新增）

```cpp
namespace cinux::drivers {

class RAMBlockDevice : public IBlockDevice {
public:
    RAMBlockDevice(size_t blocks, size_t block_size = 512);

    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override;

private:
    void* storage_;
    size_t block_count_;
    size_t block_size_;
};

} // namespace cinux::drivers
```

- [ ] 构造时从 kernel heap 分配内存
- [ ] read/write 直接 memcpy
- [ ] 用于 ext2 单元测试和 ramdisk 替代

### T4: 重构 Ext2 使用 IBlockDevice

**文件**: `kernel/fs/ext2.hpp`, `kernel/fs/ext2.cpp`

**改动前**:
```cpp
class Ext2 : public FileSystem {
    cinux::drivers::ahci::AHCI& ahci_;
    uint8_t port_index_;
    // ...
};
```

**改动后**:
```cpp
class Ext2 : public FileSystem {
    cinux::drivers::IBlockDevice* dev_;
    // ...
};
```

- [ ] 构造函数改为 `Ext2(IBlockDevice* dev)`
- [ ] `read_block()` / `write_block()` 改为调用 `dev_->read_blocks()`
- [ ] 移除内部的 DMA buffer 管理（IBlockDevice 实现负责）
- [ ] 移除 `dma_buf_phys_` / `dma_buf_virt_` / `dma_ready_` 成员
- [ ] 确保 `dma_buf_virt()` 的调用方改用 `read_block()` 返回的数据

### T5: 更新启动流程

**文件**: `kernel/main.cpp`

当前：
```cpp
// PCI -> AHCI init -> Ext2(AHCI, port)
cinux::drivers::ahci::AHCI ahci;
ahci.init(pci_dev);
auto* ext2 = new Ext2(ahci, port_index);
```

改为：
```cpp
// PCI -> AHCI init -> AHCIBlockDevice -> Ext2(block_dev)
cinux::drivers::ahci::AHCI ahci;
ahci.init(pci_dev);
auto* blk_dev = new AHCIBlockDevice(ahci, port_index);
auto* ext2 = new Ext2(blk_dev);
```

- [ ] 更新 main.cpp 中的对象构造顺序
- [ ] 更新 ramdisk mount 路径（如果需要）

### T6: 单元测试

**文件**: `kernel/test/test_block_device.cpp`

- [ ] RAMBlockDevice 读写正确
- [ ] Ext2 + RAMBlockDevice 组合测试（挂载 + 查找文件）
- [ ] AHCIBlockDevice 基本读写（需 QEMU 硬件）

## 产出物

- [ ] `kernel/drivers/block_device.hpp` — IBlockDevice 接口
- [ ] `kernel/drivers/ahci/ahci_block_device.hpp` / `.cpp` — AHCI 适配器
- [ ] `kernel/drivers/ram_block_device.hpp` — 测试用内存块设备
- [ ] `kernel/fs/ext2.hpp` / `.cpp` — 解耦重构
- [ ] `kernel/main.cpp` — 启动流程更新
- [ ] `kernel/test/test_block_device.cpp` — 单元测试
- [ ] 编译通过 + QEMU 启动 + ext2 正常读写
