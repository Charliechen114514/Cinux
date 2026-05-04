# 018-3 通读：AddressSpace 测试精讲——从 QEMU 集成到 Host 单元

## 概览

本文是 tag `018_mm_address_space` 三篇通读教程的第三篇，精讲测试代码。Cinux 为 `AddressSpace` 准备了两套测试：QEMU 集成测试（`kernel/test/test_address_space.cpp`，308 行）直接操作真实硬件；Host 单元测试（`test/unit/test_address_space.cpp`，771 行）用 Mock 完整复刻内核逻辑。QEMU 测试验证 CR3 切换和真实页表遍历，Host 测试验证算法正确性并检查 PMM 分配/释放计数来检测泄漏。

## 架构图

```
    测试架构:

    ┌──────────────────────────────────────────────┐
    │  QEMU 集成测试 (kernel/test/)                │
    │  真实 PMM + VMM + CR3 + 页表硬件             │
    │  11 个测试: init → construct → map/unmap     │
    │             → isolation → activate → destroy │
    ├──────────────────────────────────────────────┤
    │  Host 单元测试 (test/unit/)                  │
    │  MockPMM + sim_memory + TestVMM              │
    │  + TestAddressSpace (复刻内核逻辑)            │
    │  20 个测试: 构造×4 析构×2 移动×4 init×1       │
    │             map/unmap×3 隔离×2 activate×1     │
    │             子树释放×2 内核保护×1              │
    └──────────────────────────────────────────────┘
```

## 代码精讲

### 第一部分：QEMU 集成测试

#### Test 1-3：冒烟测试——初始化与构造基本正确性

```cpp
// Test 1: init_kernel saves a valid kernel PML4
namespace test_as_init {
void test_init_kernel_pml4() {
    TEST_ASSERT_NE(cinux::mm::AddressSpace::kernel_pml4(), 0u);
}
}

// Test 2: construction creates a distinct PML4 root
namespace test_as_construct {
void test_distinct_pml4() {
    cinux::mm::AddressSpace as;
    TEST_ASSERT_NE(as.pml4_phys(), 0u);
    TEST_ASSERT_NE(as.pml4_phys(), cinux::mm::AddressSpace::kernel_pml4());
}
}

// Test 3: two instances have different roots
namespace test_as_two_roots {
void test_different_roots() {
    cinux::mm::AddressSpace as1;
    cinux::mm::AddressSpace as2;
    TEST_ASSERT_NE(as1.pml4_phys(), as2.pml4_phys());
}
}
```

三个最基本的冒烟测试。Test 1 确认 `init_kernel()` 在 `main_test.cpp` 中已被调用。Test 2 确认构造分配了 PML4 且不等于内核 PML4。Test 3 确认两个实例各自拥有不同的物理 PML4 地址。每个测试用独立命名空间包裹，避免变量名冲突。

#### Test 4-6：map/unmap/translate 基本操作

Test 4 在一个 AddressSpace 中 map 虚拟地址 `0x20000000` 到 PMM 分配的物理页，然后 translate 验证返回值等于物理地址——证明四级页表遍历建立了完整的 PML4 -> PDPT -> PD -> PT 链条。Test 5 在 map 后 unmap 再 translate，确认返回 0。Test 6 对从未 map 的地址 translate，确认返回 0。三个测试覆盖了 map/unmap/translate 的正向和负向路径。测试结束时手动 `free_page` 归还 PMM，保持资源平衡。

#### Test 7：核心里程碑——跨地址空间隔离

```cpp
namespace test_as_isolation {
void test_cross_space_isolation() {
    cinux::mm::AddressSpace as1;
    cinux::mm::AddressSpace as2;
    uint64_t virt = 0x20020000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = as1.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(as1.translate(virt), phys);
    TEST_ASSERT_EQ(as2.translate(virt), 0u);   // 隔离关键断言
    g_pmm.free_page(phys);
}
}
```

这是整个 tag 018 最核心的测试。as1 映射了 `0x20020000`，translate 返回正确物理地址；as2 对同一虚拟地址 translate 返回 0。这证明 as1 的 map 只修改了自己的 PML4 树，as2 的 PML4 上看不到任何映射。这正是"每个进程拥有独立虚拟地址空间"的硬件基础。

#### Test 8-9：CR3 切换验证

Test 8 在 `activate()` 前后各读一次 CR3，确认切换后的 CR3 等于 `as.pml4_phys()`。注意最后一行 `write_cr3(saved_pml4)` 极其关键——在 `as` 析构之前必须恢复内核 PML4，否则析构释放了 CR3 正在使用的 PML4 页，CPU 用已归还的物理内存做地址翻译，三步之内 triple fault。

Test 9 在 activate 之后调用 translate，验证 CR3 已切换到新地址空间后，软件页表遍历仍然正确——因为 `translate` 委托给 VMM 的代码遍历（不依赖硬件 TLB）。

#### Test 10-11：多页映射与析构安全性

Test 10 在同一地址空间中 map 两个不同的虚拟页，验证两次 map 互不干扰。Test 11 创建 AddressSpace、映射用户页、让对象析构后检查 `kernel_pml4()` 值不变——验证 `free_subtree` 只释放 PML4[0..255] 的子树，绝不触及内核条目。如果范围算错，后续代码执行会立即 page fault。

测试入口 `run_address_space_tests()` 用 `extern "C"` 导出，`main_test.cpp` 通过函数指针直接调用。11 个测试按依赖关系排列：基础设施 -> 操作 -> 核心特性 -> 安全性。

### 第二部分：Host 单元测试基础设施

Host 测试的核心挑战是：在没有 x86-64 硬件的环境中测试"管理硬件页表"的类。解决方案是完全模拟。

#### MockPMM——位图物理内存分配器

```cpp
constexpr uint32_t MOCK_POOL_PAGES = 512;
constexpr uint64_t MOCK_POOL_BASE  = 0x2000000ULL;

struct MockPMM {
    uint8_t  bitmap[MOCK_POOL_PAGES / 8];
    uint32_t alloc_count;
    uint32_t free_count;

    MockPMM() : alloc_count(0), free_count(0) { memset(bitmap, 0, sizeof(bitmap)); }

    uint64_t alloc_page() {
        for (uint32_t i = 0; i < MOCK_POOL_PAGES; i++) {
            uint32_t byte = i / 8;
            uint32_t bit  = i % 8;
            if (!(bitmap[byte] & (1U << bit))) {
                bitmap[byte] |= static_cast<uint8_t>(1U << bit);
                alloc_count++;
                return MOCK_POOL_BASE + static_cast<uint64_t>(i) * PAGE_SIZE;
            }
        }
        return 0;
    }
    void free_page(uint64_t phys) { /* bitmap 位清除 */ }
    bool is_allocated(uint64_t phys) const { /* bitmap 位查询 */ }
};
```

512 位 bitmap 管理模拟物理页池，每个 bit 对应一个 4KB 页。`alloc_count` 和 `free_count` 是关键的泄漏检测工具——通过比较测试前后的 `free_count`，验证析构确实释放了所有页表页。`is_allocated()` 允许查询特定页的分配状态。

#### sim_memory 与 sim_virt_of——Host 内存模拟物理页

```cpp
constexpr uint32_t SIM_PAGES = 256;
alignas(4096) uint8_t sim_memory[SIM_PAGES][PAGE_SIZE];

uint8_t* sim_virt_of(uint64_t phys) {
    if (phys < MOCK_POOL_BASE) return nullptr;
    uint64_t idx = (phys - MOCK_POOL_BASE) / PAGE_SIZE;
    if (idx >= SIM_PAGES) return nullptr;
    return &sim_memory[idx][0];
}
```

`sim_memory` 是 256 页的二维数组，`alignas(4096)` 模拟真实页表页对齐。`sim_virt_of` 是 Host 版的 `phys_to_virt`——通过 `(phys - MOCK_POOL_BASE) / PAGE_SIZE` 算索引，返回对应的 Host 内存指针。物理地址在这里变成纯逻辑概念——只是 `sim_memory` 数组的索引偏移量。

#### TestVMM 与 TestAddressSpace——内核逻辑的 Host 复刻

`TestVMM` 模拟真实 VMM 的 map/unmap/translate 接口。`map_full_walk` 实现完整四级遍历，每级条目不存在时从内部 `MockPMM pmm_` 分配新页。`unmap` 简化为只清 PML4 条目（对隔离测试足够）。`translate` 只检查 PML4 级别——PML4 条目 present 就返回物理地址，否则返回 0。

`TestAddressSpace` 的构造/析构/移动/map/unmap/translate 逻辑和内核版本逐行对应，区别仅在于：构造接受 `MockPMM&` 引用而非全局 `g_pmm`，`activate()` 记录到 `last_activated_pml4_` 静态变量而非写 CR3，析构使用 `sim_virt_of` 而非 `phys_to_virt`。

#### TestFixture——每个测试的干净起点

```cpp
struct TestFixture {
    MockPMM pmm;
    TestFixture() { memset(sim_memory, 0, sizeof(sim_memory)); }

    void setup_kernel_pml4() {
        uint64_t kpml4 = pmm.alloc_page();
        auto* pml4 = reinterpret_cast<PageEntry*>(sim_virt_of(kpml4));
        memset(pml4, 0, PAGE_SIZE);
        uint64_t fake_phys = pmm.alloc_page();
        pml4[256].raw = fake_phys | FLAG_PRESENT | FLAG_WRITABLE;
        pml4[511].raw = pmm.alloc_page() | FLAG_PRESENT | FLAG_WRITABLE;
        TestAddressSpace::init_kernel(kpml4);
    }
};
```

构造函数清零 `sim_memory`，确保前一个测试不残留数据。`setup_kernel_pml4()` 模拟启动初始化：分配"内核 PML4"，在条目 256 和 511 设置假的 present 条目，让构造函数的"复制内核条目"步骤可以被验证。

### 第三部分：Host 测试用例归类讲解

Host 测试共 20 个用例，按功能分为六组。

构造类（4 个）验证 PML4 分配成功、用户空间条目全零（0 到 255 遍历检查）、内核条目与模板一致（256 到 511 逐条比较 raw 值）、两个实例的 PML4 地址不同。析构类（2 个）用 `{ ... }` 作用域触发析构，然后通过 `fx.pmm.is_allocated(pml4_phys)` 检查 PML4 页确实被归还，通过 `fx.pmm.free_count == 1` 验证无用户映射时只释放了一页。

移动类（4 个）中最有趣的是自赋值安全测试——通过 `auto& ref = as; as = std::move(ref);` 绕过 `-Wself-move` 警告，验证 `as.pml4_phys()` 没有变成 0，证明 `if (this != &other)` 守卫有效。移动赋值测试验证旧 PML4 被释放：`fx.pmm.is_allocated(pml4_2)` 返回 false，证明被覆盖对象的资源确实被回收了。

隔离类（2 个）是 Host 版核心里程碑。第一个测试与 QEMU 的 Test 7 相同——as1 映射后 as2 不可见。第二个更深入：两个 AddressSpace 在同一虚拟地址上映射不同物理页，然后 as1 的 unmap 不影响 as2——证明不仅是"映射不可见"，而且"操作也完全独立"。

子树释放类（2 个）验证析构确实回收了中间页表页。通过比较 `fx.pmm.free_count` 前后的差值，确认 map 创建的 PDPT/PD/PT 页在析构时全部被释放。多子树测试映射两个不同 PML4 槽位的地址（`0x00000000` 和 `0x8000000000`，分属 PML4[0] 和 PML4[1]），验证析构能处理多棵独立子树。

内核保护类（1 个）在用户空间 map+unmap 后检查 PML4[256] 和 PML4[511] 不变——验证用户空间的操作绝不影响内核条目。

## 设计决策

### Decision: 两套测试而非一套

**问题**：`AddressSpace` 涉及硬件交互和算法逻辑，如何高效验证？

**本项目的做法**：QEMU 集成测试验证硬件交互（CR3 切换、TLB），Host 单元测试验证算法逻辑（构造/析构/移动/隔离）。Host 测试毫秒级完成 20 个用例。

**备选方案**：全部用 QEMU 测试。

**为什么不选备选方案**：QEMU 测试需完整启动内核（GDT、IDT、PMM、VMM 全初始化），运行在秒级。Host 测试还能检查 PMM 内部状态（alloc_count、free_count），真实 PMM 被整个内核共享，测试无法独占来精确计数。

### Decision: TestAddressSpace 复刻内核逻辑而非直接链接

**问题**：Host 测试需要 AddressSpace 的逻辑，是链接还是复刻？

**本项目的做法**：完全复刻，用 MockPMM、TestVMM、sim_memory 替代真实硬件。

**为什么不直接链接**：`AddressSpace` 依赖 `g_pmm`、`g_vmm`、`write_cr3`、`phys_to_virt`（higher-half 偏移），这些在 Host 环境全部不可用。mock 的工程量和复刻相当，而且需要持续维护与内核接口的同步。复刻虽然代码重复，但逻辑完全透明，调试时能直接看到模拟层内部状态。

## 扩展方向

1. **模糊测试 map 参数** (⭐⭐)：随机生成虚拟/物理地址对，连续 map/unmap 数千次，检查 PMM 的 alloc_count 和 free_count 是否平衡。暴露 `free_subtree` 在边界情况下的泄漏。

2. **并发 AddressSpace 测试** (⭐⭐⭐)：多核就绪后，测试两个核同时操作不同 AddressSpace 的 map/unmap 是否产生竞态。页表修改的锁粒度应该在 PML4 条目级而非全局。

3. **性能基准测试** (⭐)：比较"空 AddressSpace"和"有 1000 个映射的 AddressSpace"的析构时间，验证 `free_subtree` 的递归深度与性能是否线性。

4. **测试覆盖率统计** (⭐)：在 Host 测试中启用 gcov，特别关注 `free_subtree` 的 `level == LEVEL_PT` 终止分支和移动赋值的自赋值守卫。

5. **CR3 监控** (⭐⭐)：在 QEMU 测试中用 `-d int` 或 GDB 脚本监控 CR3 变化，验证每次 `activate()` 只写一次 CR3。

## 参考资料

- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：四级页表遍历。`TestVMM::map_full_walk` 的遍历逻辑与此对应。
- Intel SDM Vol.3A Section 4.10.2：CR3 写入与 TLB 行为。QEMU Test 8 直接验证了 CR3 变化。
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — PML4 分割策略，测试中 `USER_PML4_END = 256` 的设计依据。
