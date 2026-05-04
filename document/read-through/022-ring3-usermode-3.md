# 022-3 Read-through · VMM FLAG_USER 传播 + 测试代码

## 概览

本文覆盖 tag 022 中 VMM (Virtual Memory Manager) 的 walk_level FLAG_USER 传播改造，以及两套测试代码——内核态集成测试 (test_usermode.cpp) 和 Host 端单元测试 (test/unit/test_usermode.cpp)。walk_level 的改造是这个 tag 中最关键的 bug fix 之一：没有 FLAG_USER 传播，Ring 3 代码访问任何用户页都会触发 #PF (error_code=0x05)。

测试代码覆盖了 TSS RSP0、STAR/EFER MSR、用户地址空间映射、段选择子、用户代码字节码、DF 栈等所有子系统。

## 架构图

```
VMM::map(virt, phys, flags)
  |
  +-- user_flag = flags & FLAG_USER
  |
  +-- walk_level(pml4, PML4_INDEX, true, user_flag)
  |     |
  |     +-- 分配新页表: new_page | PRESENT | WRITABLE | user_flag
  |     +-- 返回 next-level table ptr
  |
  +-- walk_level(pdpt, PDPT_INDEX, true, user_flag)
  |     +-- (同上)
  |
  +-- walk_level(pd, PD_INDEX, true, user_flag)
  |     +-- (同上)
  |
  +-- PT[index] = phys | flags   <-- 最终页表项
```

## 代码精讲

### walk_level FLAG_USER 传播 (vmm.cpp)

原来的 walk_level 签名只有三个参数：

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc);
```

改造后增加了 `user_flag` 参数，默认值为 0：

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index,
                       bool should_alloc, uint64_t user_flag = 0);
```

默认值为 0 确保了向后兼容——内核态映射不传 FLAG_USER，中间页表不会获得 user 位。

walk_level 内部有两处分配新页表的代码，都需要传播 user_flag。

**大页拆分路径**——当遇到 1GB 或 2MB 大页但需要 4KB 精细映射时：

```cpp
entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
```

大页拆分时会分配一个新的页表页，把大页拆成 512 个 4KB 小页。原来的代码只设了 PRESENT 和 WRITABLE，没有 USER。如果这个映射是用户态的，中间页表缺 user 位会导致 Ring 3 访问失败。

**空白条目分配路径**——当条目不存在且 should_alloc 为 true 时：

```cpp
entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
```

同样是加上 user_flag。这样无论走哪条路径，中间页表都会正确传播 user 位。

VMM::map() 中提取并传递 user_flag：

```cpp
bool VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
    // ...
    uint64_t user_flag = flags & FLAG_USER;

    auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
    auto* pd   = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
    auto* pt   = walk_level(pd, PD_INDEX(virt), true, user_flag);
    // ...
}
```

从 flags 中提取 FLAG_USER（bit 2），传递给每一级的 walk_level。如果 flags 包含 FLAG_USER，user_flag = FLAG_USER（非零），所有中间页表都会有 user 位。如果 flags 不包含 FLAG_USER（内核映射），user_flag = 0，行为和以前完全一样。

unmap 和 translate 路径调用 walk_level 时传 should_alloc=false，不分配新页表，所以不需要 user_flag 参数（使用默认值 0 即可）。

### 内核测试 (test/test_usermode.cpp)

内核测试在 QEMU 内运行，直接操作真实的 GDT/TSS/MSR/AddressSpace。测试分为 8 个组。

**Test 1-3: TSS RSP0**。验证 tss_set_rsp0 的基本功能——写入已知值、多次写入、使用当前 RSP。这些测试确认 GDT::tss_set_rsp0 能正确写入 TSS 的 rsp[0] 字段。

**Test 4-7: MSR 验证**。直接 rdmsr 读取 STAR (0xC0000081) 和 EFER (0xC0000080)。test_star_msr_kernel_cs 验证 STAR[63:48] = 0x08（SYSRET 内核 CS 基）。test_star_msr_syscall_cs 验证 STAR[47:32] = 0x08。test_efer_sce_bit_set 验证 EFER.SCE (bit 0) 已启用。test_sfmask_if_bit 是特殊处理——QEMU 不持久化 SFMASK，所以只验证 wrmsr 不触发 #GP。

**Test 8-12: 用户地址空间**。创建 AddressSpace 并映射用户代码页和栈页。test_map_user_code_page 在 USER_ENTRY_BASE 映射一个带 FLAG_USER 的页，然后 translate 验证映射存在。test_map_user_stack_pages 映射 4 页栈空间并验证。test_user_space_has_kernel_mapping 确认用户 PML4 的高半区是从内核复制的。

**Test 13-16: 段选择子**。通过 inline asm 读取当前 CS/DS/SS/TR，确认内核态的段选择子正确加载。CS=0x08, DS=SS=0x10, TR=0x28。

**Test 17-19: 用户代码字节码**。验证 cli=0xFA、hlt=0xF4、jmp-4=0xEBFC 的编码。

### Host 单元测试 (test/unit/test_usermode.cpp)

Host 测试在宿主机上编译运行，不链接内核代码，纯算术和常量验证。覆盖了 14 个测试组共约 40 个 TEST 用例。

关键测试组：
- **用户模式常量**：USER_ENTRY_BASE=0x400000、USER_STACK_TOP=0x7FFFFF000、USER_STACK_PAGES=4、页对齐验证
- **栈大小计算**：4 页 * 4096 = 16KB，栈基址 = 0x7FFFFB000
- **字节码编码**：CLI=0xFA、HLT=0xF4、JMP-4=0xEBFC
- **STAR MSR 计算**：SYSRET CS = 0x08+16+3 = 0x1B = GDT_USER_CODE，SYSRET SS = (0x08+8)|3 = 0x13
- **TSS 布局**：104 字节，rsp[0] 偏移=4，ist[0] 偏移=36，iomap_base 偏移=102
- **用户页标志**：PRESENT|WRITABLE|USER = 0x7，不含 PWT/PCD/HUGE/GLOBAL/NX
- **InterruptFrame 检测**：CS & 0x03 区分 Ring 0（0x08→false）和 Ring 3（0x1B→true）
- **RFLAGS**：IF=bit9=0x200，SYSRET RFLAGS=0x202（IF+bit1）
- **地址布局**：代码区(0x400000-0x401000)不与栈区(0x7FFFFB000-0x7FFFFF000)重叠

这些测试不需要 QEMU，可以直接在开发机上运行 `ctest -R usermode`，非常适合 CI 环境。

## 设计决策

### 决策：walk_level 的 user_flag 参数方式

**问题**：如何把 FLAG_USER 传播到 walk_level 的每一级页表分配？

**本项目的做法**：walk_level 增加 `uint64_t user_flag = 0` 参数，从 VMM::map() 中提取 `flags & FLAG_USER` 并传递。

**备选方案**：直接在 walk_level 内部检查 table[index] 的 FLAG_USER——如果上级页表有 USER 位，新分配的子页表也加上。或者让 walk_level 接受完整的 flags 参数。

**为什么不选备选方案**：检查上级页表的 FLAG_USER 是循环依赖——上级页表的 user 位本身就是 walk_level 设置的。传递完整 flags 会让 walk_level 知道太多调用者的细节。单独提取 user_flag 是最小化接口暴露：walk_level 只需要知道"这个映射需要用户可访问吗"。

**如果要扩展/改进**：未来支持 NX (No-Execute) 位传播时，可以用同样的模式增加 nx_flag 参数。或者重构为传递完整的权限掩码。

## 扩展方向

- 添加压力测试：映射大量用户页并验证 Ring 3 访问
- 测试 walk_level 的大页拆分路径（先映射 1GB 大页，再在同一区域映射 4KB 用户页）
- 添加 AddressSpace 的 unmap 和 remap 测试
- 实现 Host 端的 walk_level 单元测试（mock 页表结构）

## 参考资料

- Intel SDM: Vol.3A Section 4.6 — Access Rights (U/S bit in paging structures)
- Intel SDM: Vol.3A Section 4.7 — Page-Table Entries (FLAG_USER = bit 2)
- OSDev Wiki: [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- Cinux notes: document/notes/022/001_usermode_three_bugs.md — Bug 3: walk_level FLAG_USER
