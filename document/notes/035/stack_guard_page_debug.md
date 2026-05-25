---
title: Stack Guard Page
---

# 035 Stack Guard Page 排查日志

## 现象

`test_multi_term_two_terminals_independent_pipes` 在 QEMU 中卡死，无任何串口输出。换成堆分配（`new`/`delete`）后正常通过，初步怀疑栈溢出。

## 对象大小分析

| 对象 | 成员大小 | 栈分配数量 | 小计 |
|------|----------|-----------|------|
| `TerminalCell` | 12B (`char` + pad + 2×`uint32_t`) | — | — |
| `Terminal::screen_[ROWS][COLS]` | 80×25×12 = 24,000B | ×2 | ~48KB |
| `Pipe::buffer_[PIPE_BUFFER_SIZE]` | 4,096B | ×4 | ~16KB |
| **合计** | | | **~64KB** |

内核栈大小：`STACK_PAGES = 4` → 16KB。溢出约 4 倍。

## 根因定位

Guard page 检测代码已存在于 `exception_handlers.cpp`（`handle_pf`），但从未被触发。排查发现两个原因：

### 原因 1：Guard pages 从未被 unmap

- `exception_handlers.cpp:224` 注释写着 "guard region that was unmapped below `__kernel_end` at test startup"
- 但 `main_test.cpp` 中**没有任何 unmap 代码**
- 栈溢出写入的内存仍然 mapped → #PF 不触发 → 静默踩坏相邻内存

### 原因 2：#PF 没有 IST（独立栈）

- `idt.cpp:109`：`#PF` 配置 `ist=0`（无独立栈）
- 只有 `#DF`（Double Fault）使用 `ist=1`
- 当 #PF 触发时，CPU 往已溢出的栈 push 中断帧 → 再次 #PF → Double Fault → Triple Fault → QEMU 静默重启

### 原因 3：Boot stack 使用 2MB huge page 映射

- Mini kernel 通过 `identity_map_up_to()` 用 2MB huge page 映射所有物理内存
- `VMM::unmap()` 操作 4KB 粒度，无法直接 unmap 2MB huge page 中的单个 4KB 页
- 需要先 split 2MB page 为 512×4KB pages

## 修复方案（共 8 个文件）

### 1. Linker script — 添加 guard 区域

在 `__kernel_end` 和 `.stack` 之间插入 64KB NOLOAD guard 区域：

```
__kernel_end = .;
.boot_guard (NOLOAD) : ALIGN(4096) {
    __boot_guard_start = .;
    . = . + 0x10000;          /* 64 KB guard */
    __boot_guard_end = .;
}
.stack (NOLOAD) : ALIGN(4096) {
    . = . + 0x4000;           /* 16 KB stack */
    __kernel_stack_top = .;
}
```

### 2. GDT — 添加 IST2 栈

`gdt.hpp` 新增 `pf_stack_[4096]`，`gdt.cpp` 设置 `tss_.ist[1]` 指向栈顶。

### 3. IDT — #PF 使用 IST=2

```cpp
{ExceptionVector::PF, isr_pf_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt, 2},
```

### 4. VMM — 添加 split_2mb_page()

利用 `walk_level()` 已有的 huge page 自动拆分逻辑（`should_alloc=true` 时遇到 PS=1 的 entry 会分配新 PT 并展开为 512 个 4KB entry）。新方法只需 walk 到 PD 层即可触发拆分。

### 5. main_test.cpp — 运行时 unmap guard pages

```cpp
g_vmm.split_2mb_page(guard_start);  // 2MB → 512×4KB
for (addr = guard_start; addr < guard_end; addr += 4096)
    g_vmm.unmap(addr);              // unmap guard pages
```

### 6. exception_handlers.cpp — 更新检测逻辑

用 `__boot_guard_start` / `__boot_guard_end` 替代旧的 `__kernel_end` + `BOOT_GUARD_PAGES` 计算，直接检查 fault 地址是否落在 guard 范围内。

## 验证结果

```
BOOT STACK OVERFLOW DETECTED
  Fault address (CR2):  0xFFFFFFFF8108C148
  Guard page range:     [0xFFFFFFFF8107E000, 0xFFFFFFFF8108E000)
  Boot stack range:     [0xFFFFFFFF8108E000, 0xFFFFFFFF81092000)
  Current RSP:          0xFFFFFFFF8108C150
  RIP:                  0xFFFFFFFF81032D95
```

Guard page 精准捕获。fault 地址在 guard 范围内，RSP 已越过栈底。

额外发现：首个触发的不是 035 的测试，而是 031 的 `test_terminal_construction`——单个 Terminal 栈分配（24KB screen buffer）就超过 16KB boot stack。

## 关键教训

1. **注释说"已 unmap"不代表真的 unmap 了**——代码和注释不一致是最难排查的问题
2. **#PF 必须配 IST**：栈溢出场景下 #PF handler 不能用当前栈，否则直接 Double Fault
3. **2MB huge page 是隐形的 guard page 杀手**：即使 linker 预留了空间，运行时 huge page 映射让整个区域都可访问
