# M1: AHCI DMA 升级

> 将现有 AHCI 驱动从 PIO/single-PRDT 升级到 DMA scatter-gather。
> 利用 F1 的 DMA Pool 和 PRDT Builder。

## 目标

1. 使用 DMA Pool 分配 command list / FIS buffer
2. 使用 PRDT Builder 支持 scatter-gather
3. 中断驱动完成（替代轮询）
4. 实现 AHCIBlockDevice（F1 M4 的适配器）

## 任务清单

### T1: Command List / FIS Buffer 迁移到 DMA Pool

**文件**: `kernel/drivers/ahci/ahci.cpp`

当前手动 PMM alloc + VMM map → 改用 `g_dma_pool.alloc()`：
- [ ] `init()` 中 cmd_list/fis_buf 改用 DmaPool 分配
- [ ] 移除手动 PMM/VMM 代码
- [ ] 端口配置使用 DmaBuffer 的 phys/virt 字段

### T2: Scatter-Gather 读写

**文件**: `kernel/drivers/ahci/ahci.cpp`

当前单 PRDT entry → 使用 PrdtBuilder 支持多段：

```cpp
bool AHCI::read_sg(uint8_t port, uint64_t lba, uint16_t count,
                    const DmaBuffer* bufs, size_t buf_count) {
    // 构建 scatter-gather PRDT
    PrdtBuilder prdt(cmd_table->prdt, MAX_PRDT);
    for (size_t i = 0; i < buf_count; i++)
        prdt.add_buffer(bufs[i]);

    // 发送 READ DMA EXT 命令
    // ...
}
```

- [ ] read_sg / write_sg 方法
- [ ] PrdtBuilder 集成
- [ ] 支持 >4KB 的读写（自动拆分为多 PRDT entry）

### T3: 中断驱动完成

**文件**: `kernel/drivers/ahci/ahci.cpp`, `kernel/arch/x86_64/irq_handlers.cpp`

当前轮询等待 CI bit → 改为中断 + 等待队列：

- [ ] AHCI 端口启用中断（PXIE.DHRE | PXIE.DMPE | PXIE.DNSE）
- [ ] 注册 IRQ handler（通过 I/O APIC 或 PIC 路由）
- [ ] 命令发出后 block 当前 task（等待中断唤醒）
- [ ] IRQ handler 检查完成状态 → unblock 等待 task
- [ ] 超时保护（防止永久阻塞）

### T4: AHCIBlockDevice 实现

**文件**: `kernel/drivers/ahci/ahci_block_device.hpp`, `.cpp`

F1 M4 定义的 IBlockDevice 适配器：

```cpp
class AHCIBlockDevice : public IBlockDevice {
    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override { return 512; }
    void flush() override;
};
```

- [ ] read_blocks: 分配 DMA buf → read_sg → memcpy 到 buf → 释放
- [ ] write_blocks: memcpy 到 DMA buf → write_sg → 释放
- [ ] block_count: 从 IDENTIFY DEVICE 数据读取
- [ ] flush: 发送 FLUSH CACHE EXT 命令

### T5: 单元测试

- [ ] AHCI DMA 读写正确性（通过 ext2 挂载验证）
- [ ] scatter-gather 多段传输
- [ ] 中断驱动完成（不轮询）
- [ ] AHCIBlockDevice 通过 IBlockDevice 接口测试

## 产出物

- [ ] `kernel/drivers/ahci/ahci.cpp` — DMA + scatter-gather 改造
- [ ] `kernel/drivers/ahci/ahci_block_device.hpp` / `.cpp`
- [ ] 中断驱动完成机制
- [ ] ext2 通过 AHCIBlockDevice 正常挂载
