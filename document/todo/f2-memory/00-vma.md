# M1: VMA 抽象接口 + 链表后端

> 引入 VMA（Virtual Memory Area）管理，追踪每个进程的内存映射区域。
> 设计抽象接口以便后续无缝切换到红黑树后端。

## 目标

在 `AddressSpace` 中加入 VMA 追踪，为 mmap/munmap/brk/demand paging 提供基础设施。

## 现有代码

- `kernel/mm/address_space.hpp` — AddressSpace 类（仅持有 pml4_phys_，无区域追踪）
- `kernel/arch/x86_64/usermode.hpp` — `USER_ENTRY_BASE=0x400000`, `USER_STACK_TOP=0x7FFFFF000`
- `kernel/arch/x86_64/paging_config.hpp` — FLAG_PRESENT/WRITABLE/USER/COW/NX
- `kernel/proc/process.hpp` — Task 结构体持有 `AddressSpace* addr_space`

## 任务清单

### T1: VMA 结构体定义

**文件**: `kernel/mm/vma.hpp`

```cpp
namespace cinux::mm {

// VMA 权限标志（对应 mmap prot）
enum class VmaFlags : uint64_t {
    None     = 0,
    Read     = 1 << 0,     // PROT_READ
    Write    = 1 << 1,     // PROT_WRITE
    Exec     = 1 << 2,     // PROT_EXEC
    Shared   = 1 << 3,     // MAP_SHARED (vs MAP_PRIVATE)
    Anonymous= 1 << 4,     // MAP_ANONYMOUS
    Stack    = 1 << 5,     // Stack VMA (auto-grow down)
    Heap     = 1 << 6,     // Heap VMA (brk managed)
};

struct VMA {
    uint64_t start;         // 起始虚拟地址（页对齐）
    uint64_t end;           // 结束虚拟地址（页对齐，不含）
    VmaFlags flags;

    // 文件映射（匿名映射时为 nullptr）
    fs::Inode* backing_inode;
    uint64_t   file_offset;

    // 侵入式链表节点
    VMA* prev;
    VMA* next;
};

} // namespace cinux::mm
```

- [ ] 定义 VmaFlags bitmask enum class
- [ ] 定义 VMA 结构体
- [ ] 提供 `has_flag(VmaFlags, VmaFlags)` 内联检查

### T2: IVMAStore 抽象接口

**文件**: `kernel/mm/vma.hpp`

```cpp
class IVMAStore {
public:
    virtual ~IVMAStore() = default;

    // 插入 VMA（自动按地址排序/合并相邻同权限区域）
    virtual bool insert(VMA* vma) = 0;

    // 移除指定地址范围的映射
    virtual bool remove(uint64_t start, uint64_t end) = 0;

    // 查找包含 addr 的 VMA（用于 page fault 验证）
    virtual VMA* find(uint64_t addr) = 0;

    // 在 [hint, hint+length) 区域寻找空闲空间
    virtual uint64_t find_free_area(uint64_t hint, uint64_t length) = 0;

    // 遍历所有 VMA（用于 fork/execve/clear）
    virtual VMA* first() = 0;
    virtual VMA* next(VMA* current) = 0;

    // 统计
    virtual size_t count() const = 0;
};
```

- [ ] 定义 IVMAStore 纯虚接口
- [ ] 语义清晰：insert 自动合并、remove 可拆分 VMA

### T3: LinkedListVMAStore 实现

**文件**: `kernel/mm/vma.hpp`, `kernel/mm/vma.cpp`

```cpp
class LinkedListVMAStore : public IVMAStore {
public:
    bool insert(VMA* vma) override;
    bool remove(uint64_t start, uint64_t end) override;
    VMA* find(uint64_t addr) override;
    uint64_t find_free_area(uint64_t hint, uint64_t length) override;
    VMA* first() override { return head_; }
    VMA* next(VMA* cur) override { return cur->next; }
    size_t count() const override { return count_; }

private:
    VMA* head_ = nullptr;
    size_t count_ = 0;
};
```

- [ ] `insert()` — 按地址插入有序链表，合并相邻同权限 VMA
- [ ] `remove()` — 从链表摘除，必要时拆分 VMA（unmap 中间部分）
- [ ] `find()` — 线性扫描找到包含 addr 的 VMA
- [ ] `find_free_area()` — 从 hint 开始扫描间隙，找到足够大的空洞
- [ ] 从内核堆分配 VMA 对象（`new` / `kmalloc`）

### T4: AddressSpace 集成 VMA

**文件**: `kernel/mm/address_space.hpp`, `kernel/mm/address_space.cpp`

```cpp
class AddressSpace {
public:
    // 现有方法保持不变...
    bool map(uint64_t virt, uint64_t phys, uint64_t flags);
    void unmap(uint64_t virt);
    // ...

    // 新增 VMA 操作
    IVMAStore& vmas() { return *vma_store_; }
    Spinlock& vma_lock() { return vma_lock_; }

private:
    uint64_t pml4_phys_;
    IVMAStore* vma_store_;    // 新增
    Spinlock vma_lock_;       // 新增：保护 VMA 操作
};
```

- [ ] AddressSpace 构造时创建 LinkedListVMAStore
- [ ] 为 ELF 加载创建的映射同步注册 VMA（execve 改造准备）
- [ ] 为用户栈映射注册 VMA（Stack flag）
- [ ] `free_subtree()` 析构时清理 VMA store

### T5: 用户空间布局常量

**文件**: `kernel/arch/x86_64/memory_layout.hpp`（扩展）

```cpp
// 用户空间布局（扩展）
constexpr uint64_t USER_MMAP_BASE = 0x100000000ULL;   // 4 GB — mmap 起始
constexpr uint64_t USER_MMAP_END  = 0x7F0000000000ULL; // ~127 TB — mmap 上限
constexpr uint64_t USER_BRK_BASE  = 0x600000ULL;       // brk 起始（紧跟 ELF 段之后）
constexpr uint64_t USER_BRK_MAX   = 0x10000000ULL;     // 256 MB 堆上限
```

- [ ] 定义 mmap 分配区域
- [ ] 定义 brk 区域
- [ ] 保持与 USER_ENTRY_BASE / USER_STACK_TOP 兼容

### T6: 单元测试

**文件**: `kernel/test/test_vma.cpp`

- [ ] VMA insert 有序排列
- [ ] VMA find 正确查找包含地址的 VMA
- [ ] VMA remove 拆分中间段
- [ ] find_free_area 找到足够大的间隙
- [ ] 相邻同权限 VMA 自动合并
- [ ] AddressSpace 构造/析构正确管理 VMA

## 产出物

- [ ] `kernel/mm/vma.hpp` — VMA 结构体 + IVMAStore 接口 + LinkedListVMAStore
- [ ] `kernel/mm/vma.cpp` — LinkedListVMAStore 实现
- [ ] `kernel/mm/address_space.hpp` / `.cpp` — 集成 VMA store
- [ ] `kernel/arch/x86_64/memory_layout.hpp` — 用户空间布局常量
- [ ] `kernel/test/test_vma.cpp` — 单元测试
- [ ] 编译通过 + 现有测试通过
