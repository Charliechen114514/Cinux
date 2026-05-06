# M2: ASLR（地址空间随机化）

> 随机化用户态 ELF 加载地址、栈地址、mmap 区域。
> 需要内核随机数源。

## 任务清单

### T1: 内核 CSPRNG

**文件**: `kernel/lib/random.hpp`

```cpp
namespace cinux::lib {

class KRandom {
public:
    void init();  // 收集熵源

    uint32_t next32();
    uint64_t next64();
    void fill(void* buf, size_t len);

private:
    uint64_t state_;
    // 简单 xorshift128+ 或 xoshiro256**
};

extern KRandom g_random;

} // namespace cinux::lib
```

**熵源**：
- PIT tick count（低精度时间）
- APIC timer 当前值
- TSC (rdtsc)
- 网络中断时间戳（如果有）
- 键盘/鼠标中断时间戳

- [ ] KRandom 初始化（收集启动熵）
- [ ] xorshift/xoshiro 伪随机数生成
- [ ] next32/next64/fill 接口

### T2: ELF 加载地址随机化

**文件**: `kernel/proc/process.cpp`

- [ ] execve 加载 ELF 时：USER_ENTRY_BASE + random offset（页对齐）
- [ ] 保持在安全范围内（0x400000 - 0x800000 之间偏移）
- [ ] PIE (Position-Independent Executable) 支持

### T3: 栈地址随机化

- [ ] 用户栈 top = USER_STACK_TOP - (random & ~0xFFF)
- [ ] 保持栈对齐

### T4: mmap 区域随机化

- [ ] mmap 起始地址 = USER_MMAP_BASE + random offset
- [ ] 保持在 USER_MMAP_END 之下

## 产出物

- [ ] `kernel/lib/random.hpp` — KRandom
- [ ] ELF/栈/mmap 地址随机化
- [ ] 每次运行地址不同
