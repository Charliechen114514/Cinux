# 009-01: ELF Loader 加载段时自毁 ELF 头 — 内核崩溃排查

## 一、问题现象

**症状**：内核测试运行到 `Big Kernel Load Tests (009)` 时，`test_big_kernel_load` 输出 PT_LOAD[0] 信息后**突然中断**，后续测试全部消失，QEMU 退出。

```log
[RUN] test_big_kernel_load::test_load_elf_success
[ELF] Entry point: 0x0xFFFFFFFF81000000
[ELF] Program headers: 3 at offset 0x0x40
[ELF] PT_LOAD[0]: vaddr=0x0xFFFFFFFF81000000 paddr=0x0x1000000 filesz=0x0x19F0 memsz=0x0x19F0
                                          ← 输出在此截断，"Loaded segment" 消息从未出现
```

**崩溃特征**：
- 没有 `#PF`、`#GP` 等异常信息输出
- 输出在 PT_LOAD[0] 信息与 "Loaded segment" 消息之间中断
- QEMU 直接退出（`-no-reboot` 导致 triple fault 后关机）
- 之前的测试（kprintf / C++ / GDT/IDT / PMM / ATA / ELF Loader）全部通过

---

## 二、定位崩溃点

### 分析 load_elf() 执行流程

[elf_loader.cpp](../../kernel/mini/elf_loader.cpp) 中 `load_elf()` 函数的循环逻辑：

```
kprintf("PT_LOAD[%u]: ...")   ← ✅ 已输出
  ↓
边界检查 (staging buffer)     ← 不可能崩溃
  ↓
memcpy(dest, src, filesz)     ← 💥 崩溃嫌疑点 1
  ↓
memset (BSS 清零)             ← filesz == memsz，跳过
  ↓
kprintf("Loaded segment ...") ← ❌ 从未输出
```

结论：崩溃发生在 `memcpy` 调用期间或之后的循环迭代中。

---

## 三、根本原因：Staging Buffer 与 p_paddr 重叠

### 关键地址对照

```
BIG_KERNEL_LOAD_ADDR (staging buffer) = 0x1000000    (16 MB)
Big Kernel PT_LOAD[0].p_paddr         = 0x1000000    (16 MB)  ← 同一个地址！
```

大内核的 ELF 被从磁盘读到 staging buffer（0x1000000），然后 `load_elf()` 把 PT_LOAD 段复制到 `p_paddr` 指定的物理地址。但 **PT_LOAD[0] 的目标地址就是 staging buffer 自身**。

### 崩溃过程还原

```
循环 i=0:
  ┌─────────────────────────────────────────────────────┐
  │ Staging Buffer @ 0x1000000                         │
  │ ┌──────────┬──────────────────────┬─────────────┐  │
  │ │ ELF 头   │ Program Headers     │ Segment 0   │  │
  │ │ (64 B)   │ (3 × 56 B = 168 B)  │ 数据...     │  │
  │ └──────────┴──────────────────────┴─────────────┘  │
  │ src  = staging + 0x78                               │
  │ dest = p_paddr = 0x1000000 (= staging 起点!)       │
  │ size = 0x19F0                                       │
  └─────────────────────────────────────────────────────┘
                     ↓ memcpy
  ┌─────────────────────────────────────────────────────┐
  │ Staging Buffer @ 0x1000000 (已被覆盖)              │
  │ ┌─────────────────────────────┬──────────────────┐  │
  │ │ Segment 0 数据 (前 0x78 字节)│ 原始 Segment 0  │  │
  │ │ ↑ 覆盖了 ELF 头！           │                  │  │
  │ └─────────────────────────────┴──────────────────┘  │
  └─────────────────────────────────────────────────────┘

循环 i=1:
  get_phdr(ehdr, 1)
    → 读取 ehdr->e_phoff  ← 垃圾值！ELF 头已被 Segment 0 数据覆盖
    → 计算出野指针
    → 解引用野指针 → #PF → Triple Fault → QEMU 退出 💥
```

### 为什么之前的单元测试没发现？

ELF Loader 的单元测试（008）使用手工构造的小 ELF 在栈上测试，`p_paddr` 与源缓冲区不重叠，所以不会触发这个问题。只有集成测试（从磁盘读取真实的大内核 ELF）才会命中此 bug。

---

## 四、修复方案

### 修复 1: load_elf() 保存 ELF 头到本地变量

在进入加载循环**之前**，将 ELF 头字段和所有 Program Header 拷贝到栈上：

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    // 验证 ELF 头
    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf_src);

    // ★ 在任何段复制之前保存头信息
    const uint64_t saved_entry     = ehdr->e_entry;
    const uint16_t saved_phnum     = ehdr->e_phnum;
    const uint64_t saved_phoff     = ehdr->e_phoff;
    const uint16_t saved_phentsize = ehdr->e_phentsize;

    // ★ 拷贝所有 Program Header 到本地数组
    Elf64_Phdr saved_phdrs[16];
    for (uint16_t i = 0; i < saved_phnum; i++) {
        saved_phdrs[i] = *get_phdr(ehdr, i);  // 逐个深拷贝
    }

    // 后续循环只读 saved_phdrs[]，不访问 staging buffer 的头
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const Elf64_Phdr& phdr = saved_phdrs[i];
        // ... memmove(dest, src, phdr.p_filesz) ...
    }

    return saved_entry;  // 使用保存的 entry，不回读 ELF 头
}
```

### 修复 2: memcpy → memmove

当 `p_paddr` 与 staging buffer 重叠时，`memcpy`（语义上要求不重叠）是未定义行为。改为 `memmove`：

```cpp
// ❌ 重叠时是 UB
memcpy(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);

// ✅ 正确处理重叠
memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
```

### 修复 3: 测试避免二次调用 load_elf()

`test_entry_address` 原本再次调用 `load_elf()`，但此时 staging buffer 已被段数据覆盖，ELF magic 不复存在。改为复用前一个测试保存的结果：

```cpp
static uint64_t g_loaded_entry = 0;  // 共享变量

namespace test_big_kernel_load {
    void test_load_elf_success() {
        uint64_t entry = load_elf(...);
        g_loaded_entry = entry;  // 保存供后续测试使用
    }
}

namespace test_big_kernel_entry {
    void test_entry_address() {
        TEST_ASSERT_EQ(g_loaded_entry, BIG_KERNEL_LOAD_ADDR);  // 不再重复调用 load_elf
    }
}
```

---

## 五、修复后测试输出

```log
=== Big Kernel Load Tests (009) ===
  Reading 512 sectors from LBA 848 to 0x0x1000000...
[RUN] test_big_kernel_elf_magic::test_elf_magic
[PASS] test_big_kernel_elf_magic::test_elf_magic
[RUN] test_big_kernel_crc32::test_crc32_matches
  CRC32: stored=0xdfc9c7aa computed=0xdfc9c7aa (elf_end=28056)
[PASS] test_big_kernel_crc32::test_crc32_matches
[RUN] test_big_kernel_load::test_load_elf_success
[ELF] PT_LOAD[0]: vaddr=0x0xFFFFFFFF81000000 paddr=0x0x1000000 filesz=0x0x19F0 memsz=0x0x19F0
[ELF] Loaded segment 0: 0x0x1000 -> 0x0x1000000 (6640 bytes, BSS 0 bytes)
[ELF] PT_LOAD[1]: vaddr=0x0xFFFFFFFF81002000 paddr=0x0x1002000 filesz=0x0x0 memsz=0x0x5000
[ELF] Loaded segment 1: 0x0x1000 -> 0x0x1002000 (0 bytes, BSS 20480 bytes)
[ELF] All PT_LOAD segments loaded.
  Entry point: 0x0x1000000
[PASS] test_big_kernel_load::test_load_elf_success
[RUN] test_big_kernel_entry::test_entry_address
[PASS] test_big_kernel_entry::test_entry_address
[RUN] test_big_kernel_first_insn::test_first_instruction_is_cli
[PASS] test_big_kernel_first_insn::test_first_instruction_is_cli

=== Tests: 35 passed, 0 failed ===
=== ALL TESTS PASSED (exit code 0) ===
```

现在所有 3 个 PT_LOAD 段（包括之前的 bug 导致只加载了第 1 个就崩溃的问题）都能正确处理，2 个 PT_LOAD 段 + 1 个非 PT_LOAD 段全部通过。

---

## 六、经验教训

### 1. In-place 加载必须先保存元数据

ELF loader 把段复制到 `p_paddr` 时，`p_paddr` 可能与源缓冲区重叠。这种 **就地加载 (in-place loading)** 场景要求在修改缓冲区之前，先把迭代所需的所有头信息保存到独立的存储中。

```
❌ 边读边写同一个缓冲区 → 自毁
✅ 先保存头 → 再安全覆盖
```

### 2. memcpy 的重叠语义

`memcpy` 要求源和目标不重叠（`__restrict__` 语义）。当目标可能等于源时，应使用 `memmove`。虽然本项目手写的 `memcpy` 恰好是正向逐字节拷贝（`dest < src` 时不会出错），但依赖实现细节是未定义行为。

### 3. 集成测试与单元测试的互补

| 测试类型 | 覆盖场景 | 本案例 |
|----------|----------|--------|
| 单元测试 | 小规模、隔离逻辑 | 手工 ELF 在栈上，地址不重叠 → **无法发现** |
| 集成测试 | 真实数据流、端到端 | 真实大内核 ELF，地址恰好重叠 → **暴露 bug** |

### 4. 排查 "输出截断" 类崩溃的思路

当内核测试输出在某一行中断时：

1. 确认截断发生在两条 `kprintf` 之间的**哪段代码**
2. 检查截断点附近的**内存写入操作**
3. 思考写入目标地址是否可能**破坏正在使用的数据结构**
4. 画出地址映射图，寻找重叠区域

---

## 相关文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/mini/elf_loader.cpp` | 加载前保存 ELF 头 + Program Header 到本地；`memcpy` → `memmove` |
| `kernel/mini/test/test_big_kernel_load.cpp` | `test_entry_address` 复用共享变量，不再二次调用 `load_elf()` |

---

## 内存布局图

```
Big Kernel ELF 加载前（Staging Buffer 完整）:

0x1000000  ┌──────────────┐
           │ ELF Header   │ e_ident, e_entry, e_phoff, e_phnum ...
           ├──────────────┤
           │ Phdr[0]      │ PT_LOAD: paddr=0x1000000
           │ Phdr[1]      │ PT_LOAD: paddr=0x1002000
           │ Phdr[2]      │ (non-PT_LOAD)
           ├──────────────┤
           │ .text data   │ ← Segment 0 文件内容
           │ ...          │
           └──────────────┘

Big Kernel ELF 加载后（Staging Buffer 被 Segment 0 覆盖）:

0x1000000  ┌──────────────┐
           │ Segment 0    │ ← 第一条指令: cli (0xFA)
           │ .text data   │
           │ ...          │
           ├──────────────┤
           │ (原 Phdr 区域)│ ← 已被覆盖，内容无意义
           │ ...          │
           └──────────────┘

0x1002000  ┌──────────────┐
           │ BSS (零填充) │ ← Segment 1: memmove/memset 清零
           │ ...          │
           └──────────────┘
```
