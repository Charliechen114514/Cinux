# M3: NVMe 驱动

> 现代 SSD 协议。PCIe 设备，Admin Queue + I/O Queue。
> 高性能存储接口，实现 IBlockDevice。

## 依赖

- F1 IBlockDevice 接口
- F1 DMA Pool + PRDT
- F4 I/O APIC（MSI-X 中断）

## 任务清单

### T1: NVMe 控制器初始化

**文件**: `kernel/drivers/nvme/nvme.hpp`, `kernel/drivers/nvme/nvme.cpp`

```cpp
namespace cinux::drivers::nvme {

struct NVmeRegs {
    uint32_t cap;        // Controller Capabilities
    uint32_t vs;         // Version
    uint32_t intms;      // Interrupt Mask Set
    uint32_t intmc;      // Interrupt Mask Clear
    uint32_t cc;         // Controller Configuration
    uint32_t csts;       // Controller Status
    uint32_t nssr;       // NVM Subsystem Reset
    uint32_t aqa;        // Admin Queue Attributes
    uint64_t asq;        // Admin SQ Base Address
    uint64_t acq;        // Admin CQ Base Address
};

class NVMeController {
public:
    void init(const pci::PCIDevice& pci_dev);

    // 识别控制器
    void identify_controller();

    // 获取 namespace 信息
    void identify_namespace(uint32_t nsid);

    uint32_t namespace_count() const;
    uint64_t namespace_size(uint32_t nsid) const;

private:
    volatile NVmeRegs* regs_;
    DmaBuffer admin_sq_buf_;
    DmaBuffer admin_cq_buf_;
    // ...
};

} // namespace cinux::drivers::nvme
```

**初始化流程**:
1. PCI 设备发现（class=0x01, subclass=0x08）
2. 映射 BAR0/BAR1 MMIO 寄存器
3. 检查 CAP 寄存器（队列大小、页大小）
4. 配置 Admin Queue（AQA/ASQ/ACQ）
5. 使能控制器（CC.EN = 1，等待 CSTS.RDY = 1）
6. 发送 Identify Controller 命令
7. 枚举 Namespace

- [ ] PCI 发现 + MMIO 映射
- [ ] 控制器使能序列
- [ ] Admin Queue 创建

### T2: NVMe Submission/Completion Queue

```cpp
struct NVmeSQEntry {  // 64 bytes
    uint32_t cdw0;     // Command Dword 0 (opcode + flags)
    uint32_t nsid;     // Namespace ID
    uint64_t reserved[2];
    uint64_t prp1;     // PRP Entry 1（物理地址）
    uint64_t prp2;     // PRP Entry 2（用于 >2 页的传输）
    uint32_t cdw10..15;
};

struct NVmeCQEntry {  // 16 bytes
    uint32_t cdw0;     // Command Specific
    uint32_t reserved;
    uint16_t sq_head;  // SQ Head Pointer
    uint16_t sq_id;    // SQ Identifier
    uint16_t cq_id;    // CQ Identifier
    uint16_t status;   // Phase + Status Code
};
```

- [ ] SQ/CQ 环形缓冲区管理
- [ ] Admin SQ 发送 Identify 命令
- [ ] Admin CQ 轮询/中断完成

### T3: I/O Queue + 读写命令

```cpp
class NVMeNamespace : public IBlockDevice {
public:
    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override;

private:
    NVMeController* ctrl_;
    uint32_t nsid_;
    uint64_t nsze_;    // namespace size（块数）
    uint32_t lba_size_; // LBA 大小（通常 512 或 4096）
    uint32_t io_sq_id_;
    uint32_t io_cq_id_;
};
```

**NVMe Read 命令** (opcode 0x02):
- PRP1/PRP2 指向数据缓冲区物理地址
- 对于 >2 页的传输需要 PRP List（通过 DMA Pool 分配）

- [ ] 创建 I/O SQ/CQ
- [ ] Read/Write 命令构建和提交
- [ ] PRP List 处理（大块传输）
- [ ] 中断驱动完成

### T4: 单元测试

- [ ] NVMe 控制器初始化（QEMU nvme 设备）
- [ ] Namespace 枚举
- [ ] 读写正确性验证
- [ ] IBlockDevice 接口测试

## 产出物

- [ ] `kernel/drivers/nvme/nvme.hpp` / `.cpp` — NVMe 控制器
- [ ] `kernel/drivers/nvme/nvme_block.hpp` / `.cpp` — NVMeNamespace
- [ ] `kernel/drivers/nvme/CMakeLists.txt`
- [ ] QEMU `-drive file=disk.img,if=none,id=nvme0 -device nvme,drive=nvme0` 验证
