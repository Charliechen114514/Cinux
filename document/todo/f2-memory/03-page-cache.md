# M4: Page Cache

> 基于 inode + offset 的哈希表页缓存。
> 缓存文件内容的内存页，为文件映射（mmap）和文件系统性能优化提供基础。

## 目标

实现内核级 Page Cache，所有文件 I/O 经过缓存层。
消除 ext2 的逐块 DMA 读取，复用已缓存的页。

## 现有代码

- `kernel/mm/pmm.hpp` — alloc_page 物理页分配
- `kernel/mm/vmm.hpp` — 页表映射
- `kernel/fs/inode.hpp` — Inode 结构体（有 ino 字段）
- `kernel/proc/sync.hpp` — Spinlock

## 任务清单

### T1: Page Cache 核心数据结构

**文件**: `kernel/mm/page_cache.hpp`, `kernel/mm/page_cache.cpp`

```cpp
namespace cinux::mm {

struct CachedPage {
    uint64_t    page_phys;      // 物理地址
    uint64_t    page_virt;      // 内核虚拟地址（访问内容）
    fs::Inode*  inode;          // 所属 inode
    uint64_t    offset;         // 文件内偏移（页对齐）
    bool        dirty;          // 脏页标记
    bool        valid;          // 数据已加载
    uint32_t    ref_count;      // 引用计数

    // 哈希链
    CachedPage* hash_next;
    CachedPage* hash_prev;

    // 脏页链
    CachedPage* dirty_next;
};

class PageCache {
public:
    void init(size_t max_pages);

    // 查找已缓存页（命中返回，未命中返回 nullptr）
    CachedPage* lookup(fs::Inode* inode, uint64_t offset);

    // 获取页（命中直接返回，未命中则分配 + 从文件加载）
    CachedPage* get_page(fs::Inode* inode, uint64_t offset);

    // 标记脏页
    void mark_dirty(CachedPage* page);

    // 写回脏页
    int writeback(CachedPage* page);

    // 写回所有脏页
    void writeback_all();

    // 释放页（引用计数归零时）
    void release(CachedPage* page);

    // 统计
    size_t cached_pages() const;
    size_t dirty_pages() const;
    size_t hit_count() const;
    size_t miss_count() const;

private:
    // 哈希函数：(inode_ptr ^ offset) >> PAGE_SHIFT
    uint64_t hash_key(fs::Inode* inode, uint64_t offset);
    CachedPage* bucket(uint64_t key);

    static constexpr size_t HASH_BUCKETS = 256;
    CachedPage* buckets_[HASH_BUCKETS]{};
    Spinlock lock_;

    // 脏页链表头
    CachedPage* dirty_list_ = nullptr;

    // 统计
    size_t max_pages_;
    size_t total_pages_ = 0;
    size_t hits_ = 0;
    size_t misses_ = 0;
};

extern PageCache g_page_cache;

} // namespace cinux::mm
```

- [ ] CachedPage 结构体
- [ ] PageCache 哈希表（256 buckets，链地址法）
- [ ] `lookup()` — O(1) 查找
- [ ] `get_page()` — 查找 + 未命中时分配加载
- [ ] 引用计数管理
- [ ] 脏页链表追踪

### T2: Page Cache 填充（从文件读取）

```cpp
CachedPage* PageCache::get_page(Inode* inode, uint64_t offset) {
    auto* page = lookup(inode, offset);
    if (page) {
        hits_++;
        page->ref_count++;
        return page;
    }

    misses_++;
    // 分配物理页
    uint64_t phys = g_pmm.alloc_page();
    // 映射到内核虚拟地址
    uint64_t virt = map_temporary(phys);

    // 创建 CachedPage
    page = new CachedPage{phys, virt, inode, offset, false, false, 1, ...};

    // 从文件读取数据到页面
    // 通过 inode->ops->read() 读取一页数据
    int64_t n = inode->ops->read(inode, offset, (void*)virt, PAGE_SIZE);
    page->valid = (n >= 0);
    page->dirty = false;

    // 插入哈希表
    insert_hash(page);
    return page;
}
```

- [ ] 页分配 + 临时映射
- [ ] 调用 inode->ops->read() 填充数据
- [ ] 处理文件末尾部分页（零填充剩余）
- [ ] 插入哈希表

### T3: 脏页写回

```cpp
int PageCache::writeback(CachedPage* page) {
    if (!page->dirty) return 0;
    // 通过 inode->ops->write() 写回一页
    int64_t n = page->inode->ops->write(
        page->inode, page->offset,
        (const void*)page->page_virt, PAGE_SIZE);
    if (n > 0) page->dirty = false;
    return (n > 0) ? 0 : -1;
}
```

- [ ] writeback 单页
- [ ] writeback_all 遍历脏页链
- [ ] 从脏页链移除已写回的页

### T4: Page Cache 初始化

**文件**: `kernel/main.cpp`

- [ ] 在 PMM/VMM 初始化后、文件系统挂载前调用 `g_page_cache.init(max_pages)`
- [ ] max_pages 根据可用物理内存计算（如总页数的 10%）

### T5: 单元测试

**文件**: `kernel/test/test_page_cache.cpp`

- [ ] 基本查找：插入后 lookup 命中
- [ ] 未命中分配 + 填充
- [ ] 引用计数：release 后 ref_count 递减
- [ ] 脏页标记 + 写回
- [ ] 统计：hit/miss 计数正确
- [ ] 相同 inode+offset 第二次 lookup 命中

## 产出物

- [ ] `kernel/mm/page_cache.hpp` — CachedPage + PageCache 类
- [ ] `kernel/mm/page_cache.cpp` — PageCache 实现
- [ ] `kernel/mm/CMakeLists.txt` — 加入 page_cache.cpp
- [ ] `kernel/main.cpp` — 初始化调用
- [ ] `kernel/test/test_page_cache.cpp` — 单元测试
- [ ] 编译通过
