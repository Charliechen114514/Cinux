# M7: 伙伴系统 + Slab 分配器

> 替换当前 PMM bitmap + Heap first-fit 为分层分配器。
> 伙伴系统管理物理页，Slab 高效分配小对象。

## 目标

当前内核内存管理：
- PMM：bitmap 单页分配（无连续页高效分配）
- Heap：first-fit 链表（碎片化严重，无对象复用）

升级为：
```
PMM（物理页管理）
  → Buddy System（2^n 页块分配/合并）
    → Slab Allocator（小对象高效复用，如 task_struct、inode、dentry）
      → Heap（通用分配）
```

## 任务清单

### T1: Buddy System 替换 PMM

**文件**: `kernel/mm/buddy.hpp`, `kernel/mm/buddy.cpp`

```cpp
class BuddyAllocator {
public:
    void init(uint64_t base_phys, uint64_t total_pages);

    // 分配 2^order 个连续物理页
    uint64_t alloc(uint32_t order);

    // 释放
    void free(uint64_t phys, uint32_t order);

    // 分配单个页（order=0）
    uint64_t alloc_page();
    void free_page(uint64_t phys);

    // 连续页分配（任意数量，内部向上取整到 2^order）
    uint64_t alloc_pages(uint64_t count);
    void free_pages(uint64_t phys, uint64_t count);

    // 统计
    uint64_t free_pages() const;
    uint64_t total_pages() const;

private:
    static constexpr int MAX_ORDER = 11;  // 最大 2^11 = 2048 页 = 8MB
    struct FreeBlock {
        FreeBlock* next;
    };
    FreeBlock* free_lists_[MAX_ORDER + 1];
    uint64_t   base_;
    uint64_t   total_pages_;
    uint64_t   free_count_;
    Spinlock   lock_;
};
```

**伙伴系统核心**：
- 维护 MAX_ORDER+1 个空闲链表（order 0..11）
- alloc：从对应 order 链表取，空则向上拆分
- free：释放后尝试与 buddy 合并
- buddy 地址计算：`phys ^ (1 << (order + PAGE_SHIFT))`

- [ ] BuddyAllocator 实现
- [ ] 替换 PMM 内部实现（保持 PMM 外部接口不变）
- [ ] alloc_page/free_page 向后兼容
- [ ] 连续页分配性能验证

### T2: Slab 分配器

**文件**: `kernel/mm/slab.hpp`, `kernel/mm/slab.cpp`

```cpp
class SlabAllocator {
public:
    void init();

    // 从 slab 分配对象
    void* alloc(size_t size);

    // 释放对象
    void free(void* ptr);

    // 创建专用 slab 缓存（为特定类型优化）
    SlabCache* create_cache(const char* name, size_t obj_size,
                            void (*ctor)(void*), void (*dtor)(void*));

    // 从专用缓存分配
    void* cache_alloc(SlabCache* cache);
    void cache_free(SlabCache* cache, void* obj);

private:
    struct SlabCache {
        char         name[32];
        size_t       obj_size;
        uint32_t     objs_per_slab;
        Slab*        partial;   // 部分满的 slab
        Slab*        full;      // 完全满的 slab
        Slab*        empty;     // 完全空的 slab
        void (*ctor)(void*);
        void (*dtor)(void*);
    };

    struct Slab {
        SlabCache* cache;
        Slab*      next;
        uint32_t   inuse;
        void*      freelist;
    };

    // 通用缓存（按 2^n 大小）
    SlabCache general_caches_[GENERAL_CACHE_COUNT];
};
```

**Slab 设计**：
- 通用缓存：8, 16, 32, 64, 128, 256, 512, 1024, 2048 字节（9 个）
- 专用缓存：为 Task、Inode、Dentry、Pipe 等高频结构创建
- 每个 Slab = 1 个物理页，内含多个对象
- 空闲链表在对象自身空间中（零额外内存开销）

- [ ] SlabCache + Slab 实现
- [ ] 通用缓存初始化
- [ ] kmalloc/kfree 改为走 slab
- [ ] 专用缓存创建接口

### T3: kmalloc/kfree 迁移

**文件**: `kernel/mm/heap.hpp` / `.cpp`

- [ ] kmalloc(size) → SlabAllocator::alloc(size)
- [ ] kfree(ptr) → SlabAllocator::free(ptr)
- [ ] 大块分配（>2048 字节）→ fallback 到 Buddy 直接分配
- [ ] 保留旧 Heap 接口作为兼容层，内部转发

### T4: 专用 Slab 缓存

为高频内核对象创建专用缓存：

| 对象 | 大小 | 缓存名 |
|------|------|--------|
| Task | ~600B | task_cache |
| Inode | ~128B | inode_cache |
| Dentry | ~300B | dentry_cache |
| VMA | ~64B | vma_cache |
| CachedPage | ~64B | page_cache_entry |
| Pipe | ~4KB+ | pipe_cache |

- [ ] 每个高频对象注册专用缓存
- [ ] 构造函数/析构函数初始化

### T5: 性能验证

- [ ] Buddy 分配/释放时间对比 bitmap
- [ ] Slab 分配时间对比 first-fit heap
- [ ] 碎片率对比（长时间运行后空闲页统计）
- [ ] 大量小对象分配/释放场景

## 产出物

- [ ] `kernel/mm/buddy.hpp` / `.cpp` — 伙伴系统
- [ ] `kernel/mm/slab.hpp` / `.cpp` — Slab 分配器
- [ ] kmalloc/kfree 迁移到 Slab
- [ ] 专用对象缓存
- [ ] PMM 接口向后兼容
- [ ] 性能对比数据
