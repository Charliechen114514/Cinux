# M3: DMA Infrastructure

> 统一的 DMA 缓冲区管理和 PRDT 构建工具库。
> 参考 mini kernel (`kernel/mini/driver/ata.cpp`) 的 DMA 实现，提升到 big kernel。

## 目标

为所有需要 DMA 的驱动（AHCI、NVMe、VirtIO、网卡）提供共享基础设施：
1. 物理连续缓冲区的统一分配/释放
2. PRDT（Physical Region Descriptor Table）构建工具
3. DMA 地址映射辅助

## 现有代码

- `kernel/mm/pmm.hpp` — `alloc_page()` / `alloc_pages(n)` 分配物理页
- `kernel/mm/vmm.hpp` — `map(virt, phys, flags)` 映射虚拟地址
- `kernel/arch/x86_64/memory_layout.hpp` — `KMEM_DMA_BASE` 区域
- `kernel/drivers/ahci/ahci.cpp` — 当前 DMA 用法：手动 PMM alloc + VMM map
- `kernel/mini/driver/ata.cpp` — 512-entry PRDT + Bus Master DMA 参考
- `kernel/drivers/pci/pci.hpp` — `Prd` 结构体已定义

## 任务清单

### T1: DMA Buffer Pool

**文件**: `kernel/drivers/dma/dma_pool.hpp`, `kernel/drivers/dma/dma_pool.cpp`

```cpp
namespace cinux::drivers::dma {

struct DmaBuffer {
    uint64_t phys;     // 物理地址（给硬件）
    uint64_t virt;     // 虚拟地址（给 CPU）
    size_t   size;     // 字节大小
};

class DmaPool {
public:
    void init(uint64_t virt_base, size_t pool_size);

    // 分配（从 pool 或 fallback 到 PMM）
    DmaBuffer alloc(size_t bytes);         // 对齐到页
    DmaBuffer alloc_aligned(size_t bytes, size_t align);

    // 释放
    void free(const DmaBuffer& buf);

    // 状态查询
    size_t used() const;
    size_t available() const;

private:
    // 简单 bitmap 管理预分配页
    uint64_t virt_base_;
    uint64_t phys_base_;
    size_t   total_pages_;
    uint8_t* bitmap_;   // 1 = allocated
    Spinlock lock_;
};

extern DmaPool g_dma_pool;

} // namespace cinux::drivers::dma
```

- [ ] DmaBuffer 结构体（phys + virt + size 三元组）
- [ ] DmaPool 类：预分配一块连续虚拟地址区域，bitmap 管理页级分配
- [ ] `init()` 在 `KMEM_DMA_BASE` 区域映射预分配页
- [ ] `alloc()` 返回 DmaBuffer，失败返回 `{0, 0, 0}`
- [ ] `free()` 标记 bitmap 位
- [ ] 全局单例 `g_dma_pool`
- [ ] pool 不足时 fallback 到 PMM.alloc_page() + VMM.map()

### T2: PRDT Builder

**文件**: `kernel/drivers/dma/prdt.hpp`, `kernel/drivers/dma/prdt.cpp`

```cpp
namespace cinux::drivers::dma {

struct PrdEntry {
    uint32_t address;       // 物理地址低 32 位
    uint32_t address_hi;    // 物理地址高 32 位（64-bit）
    uint32_t byte_count;    // 传输字节数（0 = 4GB）
    uint32_t reserved;
} __attribute__((packed));

class PrdtBuilder {
public:
    PrdtBuilder(PrdEntry* entries, size_t max_entries);

    // 添加一个连续段
    bool add_segment(uint64_t phys, uint32_t bytes);

    // 从 DmaBuffer 添加
    bool add_buffer(const DmaBuffer& buf);

    // 从 scatter-gather 列表添加
    size_t add_scatter_gather(const DmaBuffer* bufs, size_t count);

    size_t entry_count() const;
    uint32_t total_bytes() const;

private:
    PrdEntry* entries_;
    size_t    max_entries_;
    size_t    count_{0};
    uint32_t  total_bytes_{0};
};

} // namespace cinux::drivers::dma
```

- [ ] PrdEntry 结构体（兼容 PCI Prd，支持 64-bit 地址）
- [ ] PrdtBuilder 类：构建 scatter-gather PRDT
- [ ] `add_segment()` — 添加物理地址 + 长度
- [ ] `add_buffer()` — 从 DmaBuffer 便捷添加
- [ ] `add_scatter_gather()` — 批量添加多个 buffer
- [ ] 自动处理 >4KB 的跨段拆分

### T3: DMA 辅助函数

**文件**: `kernel/drivers/dma/dma_helpers.hpp`

```cpp
namespace cinux::drivers::dma {

// 判断地址是否在 DMA 可达范围（32-bit 兼容检查）
bool is_dma_addressable(uint64_t phys, size_t len);

// 缓冲区清零（通过虚拟地址）
void dma_zero(const DmaBuffer& buf);

// 数据拷贝
void dma_copy_to(const DmaBuffer& dst, const void* src, size_t len);
void dma_copy_from(void* dst, const DmaBuffer& src, size_t len);

} // namespace cinux::drivers::dma
```

- [ ] 地址可达性检查（32-bit 设备兼容）
- [ ] DMA 缓冲区清零
- [ ] CPU ↔ DMA 缓冲区数据拷贝

### T4: CMake 集成

**文件**: `kernel/drivers/dma/CMakeLists.txt`

- [ ] 创建 `kernel/drivers/dma/` 目录
- [ ] 添加 CMakeLists.txt，编译 dma_pool.cpp + prdt.cpp
- [ ] 加入 kernel/drivers/ 的子模块

### T5: 迁移 AHCI DMA 用法

**文件**: `kernel/drivers/ahci/ahci.cpp`

- [ ] AHCI 初始化改用 `g_dma_pool.alloc()` 分配 command list / FIS buffer
- [ ] AHCI 读写改用 `PrdtBuilder` 构建 PRDT
- [ ] 移除手动 PMM + VMM 代码

### T6: 单元测试

**文件**: `kernel/test/test_dma.cpp`

- [ ] DmaPool 分配/释放基本流程
- [ ] 连续分配不重叠
- [ ] 释放后可重新分配
- [ ] PrdtBuilder 正确构建 PRDT entries
- [ ] scatter-gather 多段正确
- [ ] dma_copy_to / dma_copy_from 数据正确

## 产出物

- [ ] `kernel/drivers/dma/dma_pool.hpp` / `.cpp` — DMA 缓冲区池
- [ ] `kernel/drivers/dma/prdt.hpp` / `.cpp` — PRDT 构建工具
- [ ] `kernel/drivers/dma/dma_helpers.hpp` — 辅助函数
- [ ] `kernel/drivers/dma/CMakeLists.txt` — 构建配置
- [ ] AHCI 驱动迁移到新 DMA 基础设施
- [ ] `kernel/test/test_dma.cpp` — 单元测试
- [ ] 编译通过 + QEMU 启动正常
