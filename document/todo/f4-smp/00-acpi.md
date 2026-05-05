# M1: ACPI 表格解析

> 解析 ACPI RSDP → XSDT → MADT/FADT/HPET。
> 获取 CPU 列表、APIC 地址、定时器信息。

## 目标

从 BIOS 提供的 ACPI 表中提取 SMP 启动所需信息。

## 任务清单

### T1: RSDP 定位

**文件**: `kernel/drivers/acpi/acpi.hpp`, `kernel/drivers/acpi/rsdp.cpp`

RSDP（Root System Description Pointer）是 ACPI 入口，定位方式：
1. 搜索 BIOS EBDA（Extended BIOS Data Area）前 1KB
2. 搜索 0xE0000 - 0xFFFFF 区域

```cpp
namespace cinux::drivers::acpi {

struct RSDP {
    char      signature[8];    // "RSD PTR "
    uint8_t   checksum;
    char      oem_id[6];
    uint8_t   revision;        // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t  rsdt_address;    // RSDT 物理地址（32-bit）
    // ACPI 2.0+ 字段：
    uint32_t  length;
    uint64_t  xsdt_address;    // XSDT 物理地址（64-bit）
    uint8_t   extended_checksum;
    uint8_t   reserved[3];
} __attribute__((packed));

// 搜索并验证 RSDP
const RSDP* find_rsdp();

} // namespace cinux::drivers::acpi
```

- [ ] find_rsdp() — EBDA + 0xE0000-0xFFFFF 搜索
- [ ] signature 验证（"RSD PTR "）
- [ ] checksum 校验（所有字节之和 mod 256 == 0）
- [ ] 优先使用 XSDT（revision >= 2），fallback 到 RSDT

### T2: SDT 头部 + XSDT/RSDT 解析

**文件**: `kernel/drivers/acpi/acpi.hpp`, `kernel/drivers/acpi/sdt.cpp`

所有 ACPI 表共有的头部：

```cpp
struct SDTHeader {
    char      signature[4];   // 表签名，如 "APIC", "FACP"
    uint32_t  length;         // 整个表长度
    uint8_t   revision;
    uint8_t   checksum;
    char      oem_id[6];
    char      oem_table_id[8];
    uint32_t  oem_revision;
    uint32_t  creator_id;
    uint32_t  creator_revision;
} __attribute__((packed));
```

```cpp
// 通过签名查找 SDT 表
const SDTHeader* find_table(const char* signature);
```

- [ ] SDTHeader 结构体
- [ ] 遍历 XSDT/RSDT 的指针数组
- [ ] find_table() 按签名查找（"APIC", "FACP", "HPET"）
- [ ] 每个表的 checksum 校验

### T3: MADT 解析

**文件**: `kernel/drivers/acpi/madt.cpp`

MADT（Multiple APIC Description Table）包含 CPU 和 APIC 信息：

```cpp
struct MADTHeader {
    SDTHeader header;
    uint32_t  local_apic_address;  // Local APIC MMIO 基地址
    uint32_t  flags;               // 1 = PCAT_COMPAT
    // 之后是变长 Interrupt Controller Structure 列表
};

// ICS 类型
enum ICS Type : uint8_t {
    ProcessorLocalAPIC = 0,
    IOAPIC = 1,
    InterruptSourceOverride = 2,
    NonMaskableInterrupt = 3,
};

struct ProcessorLocalAPICEntry {
    uint8_t  type;       // 0
    uint8_t  length;     // 8
    uint8_t  processor_id;
    uint8_t  apic_id;    // Local APIC ID（用于 IPI）
    uint32_t flags;      // bit 0 = enabled, bit 1 = online capable
};

struct IOAPICEntry {
    uint8_t  type;       // 1
    uint8_t  length;     // 12
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;  // I/O APIC MMIO 基地址
    uint32_t gsi_base;        // 全局系统中断号起始
};
```

```cpp
struct ACPIInfo {
    uint64_t  local_apic_address;
    uint64_t  ioapic_address;
    uint32_t  gsi_base;
    uint8_t   cpu_apic_ids[MAX_CPUS];  // 所有 CPU 的 APIC ID
    uint32_t  cpu_count;
    bool      has_ioapic;
};

ACPIInfo parse_madt(const MADTHeader* madt);
```

- [ ] MADT 头部解析（Local APIC 地址）
- [ ] 遍历 ICS 条目
- [ ] 提取所有 enabled CPU 的 APIC ID
- [ ] 提取 I/O APIC 地址和 GSI base
- [ ] Interrupt Source Override 解析（IRQ 重映射信息）

### T4: FADT 解析

```cpp
struct FADT {
    SDTHeader header;
    // ... 大量字段，我们只关心：
    uint32_t  pm1a_event_block;    // 电源管理寄存器
    uint32_t  pm1a_control_block;
    // HPET 地址在单独的 HPET 表中
};
```

- [ ] 解析 FADT 基本字段
- [ ] 提取电源管理寄存器地址

### T5: HPET 表解析

```cpp
struct HPETTable {
    SDTHeader header;
    uint32_t  event_timer_block_id;
    ACPIGenericAddress base_address;  // HPET MMIO 地址
    uint8_t   hpet_number;
    uint16_t  minimum_tick;
    uint8_t   page_protection;
};
```

- [ ] 从 HPET 表提取 MMIO 地址
- [ ] 传递给 F5 的 HPET 驱动

### T6: 集成到启动流程

**文件**: `kernel/main.cpp`

在 PCI 枚举之前调用 ACPI 解析：

```
现有顺序: Serial → GDT → IDT → PIC → PIT → PMM → VMM → Heap → ...
新增:                                               → ACPI → APIC → ...
```

- [ ] 在 VMM/Heap 初始化后调用 `acpi::find_rsdp()` 和表解析
- [ ] 保存 ACPIInfo 到全局变量
- [ ] 后续 Milestone 使用此信息

### T7: 单元测试

- [ ] RSDP 搜索（使用 QEMU 提供的 ACPI 表）
- [ ] MADT 解析提取 CPU 列表
- [ ] XSDT 表查找功能

## 产出物

- [ ] `kernel/drivers/acpi/acpi.hpp` — ACPI 结构体定义
- [ ] `kernel/drivers/acpi/rsdp.cpp` — RSDP 搜索
- [ ] `kernel/drivers/acpi/sdt.cpp` — XSDT/RSDT 遍历
- [ ] `kernel/drivers/acpi/madt.cpp` — MADT 解析
- [ ] `kernel/drivers/acpi/CMakeLists.txt`
- [ ] 全局 ACPIInfo 结构
- [ ] QEMU `-smp 2` 验证检测到 2 个 CPU
