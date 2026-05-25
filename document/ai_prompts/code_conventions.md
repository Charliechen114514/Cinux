# Cinux 代码风格规范

> 本文档定义了 Cinux 操作系统项目的代码编写规范。
> 所有贡献者（包括 AI）在编写代码时必须遵守本规范。

---

## 一、命名规范

### 1.1 文件命名

| 类型 | 命名规则 | 示例 |
|------|----------|------|
| C++ 头文件 | `snake_case.hpp` | `page_allocator.hpp` |
| C++ 源文件 | `snake_case.cpp` | `page_allocator.cpp` |
| 汇编头文件 | `snake_case.h` | `asm_helpers.h` |
| 汇编源文件 | `snake_case.S` | `context_switch.S` |
| 测试文件 | `test_<module>.cpp` | `test_pmm.cpp` |

### 1.2 类型命名

```cpp
// 类名：PascalCase
class PageAllocator { };

// 结构体：PascalCase
struct MemoryMapEntry { };

// 联合体：PascalCase
union PageEntry { };

// 枚举类：PascalCase
enum class TaskState { Running, Ready, Blocked };

// 枚举值：PascalCase（枚举类）或全大写（普通枚举）
enum class TaskState {
    kRunning,   // 推荐：枚举类 + k 前缀
    kReady,
};
```

### 1.3 变量命名

```cpp
// 普通变量/函数参数：snake_case
uint64_t free_page_count;
void* physical_address;

// 函数：snake_case
void* alloc_page();
void free_page(uint64_t phys);

// 成员变量：snake_case，尾随下划线（可选）
class PageAllocator {
    uint64_t total_pages_;   // 尾随下划线表示成员
    uint64_t* bitmap_;
};

// 全局变量：g_ 前缀
uint64_t g_tick_count = 0;

// 常量：k 前缀 + PascalCase
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kKernelBaseAddr = 0xFFFF800000000000;

// 宏：全大写，下划线分隔
#define MAX_TESTS 256
#define ROUND_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
```

### 1.4 命名空间命名

```cpp
// 小写，简短
namespace mm {
    class PageAllocator { };
}

// 嵌套命名空间（C++17）
namespace kernel::mm {
    class PageAllocator { };
}
```

---

## 二、注释规范

### 2.1 Doxygen 风格（文件、类、函数声明）

```cpp
/**
 * @file kernel/mm/pmm.hpp
 * @brief 物理内存管理器接口
 *
 * PMM 负责管理物理页面的分配和释放。本模块提供按页分配的
 * 基础接口，被 VMM 和 Heap 等上层模块使用。
 *
 * @note 使用前需要先调用 PMM::init()
 */

#pragma once

#include <stdint.h>

namespace mm {

/**
 * @brief 物理内存管理器
 *
 * 提供 4KB 页面的分配和释放功能。内部使用 bitmap 算法
 * 追踪已用/空闲页面。
 *
 * @note 分配的物理地址保证 4KB 对齐
 */
class PageAllocator {
public:
    /**
     * @brief 初始化物理内存管理器
     *
     * 根据 BootInfo 中的内存图初始化 bitmap。
     *
     * @param boot_info 启动信息，包含 E820 内存图
     * @return 初始化成功返回 true，失败返回 false
     * @note 必须在内核启动早期调用，且只能调用一次
     */
    static bool init(const BootInfo* boot_info);

    /**
     * @brief 分配一页物理内存
     *
     * @return 分配的物理地址，0 表示 OOM
     * @note 返回值保证 4KB 对齐
     */
    static uint64_t alloc_page();

    /**
     * @brief 释放一页物理内存
     *
     * @param phys 要释放的物理地址（必须是 4KB 对齐）
     * @note 释放未分配的页面或 double-free 会触发 panic
     */
    static void free_page(uint64_t phys);
};

} // namespace mm
```

### 2.2 函数体内注释（实现细节）

```cpp
bool PageAllocator::init(const BootInfo* boot_info) {
    // 步骤 1：解析 E820 内存图，提取可用区域
    //
    // 思路：遍历 boot_info->mmap，筛选 type=1（可用）的区域，
    //       过滤低 1MB（保留给 BIOS 和硬件），对齐到 4KB 边界。

    // 步骤 2：计算 bitmap 所需大小并分配内存
    //
    // 思路：bitmap = 总页数 / 8 字节，放在 __kernel_end 对齐后的位置。

    // 步骤 3：初始化 bitmap 为全已用状态
    //
    // 思路：memset(bitmap, 0xFF, size)，然后标记可用区域为 0。

    // 步骤 4：标记内核和 bitmap 自身占用的页面
    //
    // 思路：从 0x100000 开始到 __kernel_end，逐页标记为已用。

    return true;
}
```

### 2.3 未实现函数注释

```cpp
void PageAllocator::free_page(uint64_t phys) {
    // TODO: 实现 free_page，思路是：
    //   1. 验证 phys 是 4KB 对齐的地址
    //   2. 将 phys 转换为页索引 index = phys / 4096
    //   3. 检查 bitmap[index] 是否为 1（已用），否则 panic
    //   4. 清零 bitmap[index]
    //   5. 递增 free_page_count
}
```

---

## 三、汇编代码规范

### 3.1 AT&T 语法

```asm
# 操作数顺序：源操作数, 目标操作数
# mov src, dst     # Intel: mov dst, src

# 寄存器前缀：%
# mov %rax, %rbx   # RAX -> RBX

# 立即数前缀：$
# mov $0x1000, %rax   # 立即数 0x1000 -> RAX

# 内存寻址：offset(base, index, scale)
# mov 8(%rsp), %rdi    # [RSP+8] -> RDI
# mov (%rsp, %rax, 4), %rdx  # [RSP+RAX*4] -> RDX
```

### 3.2 汇编文件注释

```asm
/**
 * @file kernel/arch/x86_64/context_switch.S
 * @brief 上下文切换汇编实现
 *
 * 使用 callee-saved 约定保存/恢复任务上下文。
 */

.section .text

# ============================================================
# 函数名: context_switch
# 职责: 保存当前任务上下文，切换到下一个任务
# 输入: %rdi = from* (CpuContext*), %rsi = to* (CpuContext*)
# 输出: 无
# 影响: 修改 RBX, RBP, R12-R15, RSP
# ============================================================
.global context_switch
.type context_switch, @function
context_switch:

    # 步骤 1: 保存 from 的 callee-saved 寄存器
    # movq %rbx, (%rdi)           # [RBX→from->rbx]
    # movq %rbp, 8(%rdi)          # [RBP→from->rbp]

    # TODO: 步骤 2: 保存 R12-R15
    # TODO: 步骤 3: 保存 from 的 RSP
    # TODO: 步骤 4: 获取 to 的返回地址并保存到 from->rip
    # TODO: 步骤 5: 恢复 to 的寄存器
    # TODO: 步骤 6: 跳转到 to->rip

    # ret                          # [栈→RIP] 返回到 to 任务
```

---

## 四、格式化规范

### 4.1 缩进与对齐

- **缩进**：4 个空格的Tab
- **大括号**：K&R 风格，左括号不换行
- **对齐**：连续的赋值/定义按最长对齐

```cpp
// K&R 风格
if (condition) {
    do_something();
} else {
    do_other();
}

// 成员初始化列表对齐
PageAllocator::PageAllocator()
    : total_pages_(0)
    , free_pages_(0)
    , bitmap_(nullptr) {
}

// 连续变量定义对齐
uint64_t kernel_start = 0x100000;
uint64_t kernel_end   = 0x200000;
uint64_t bitmap_size  = 1024;
```

### 4.2 空行规则

- 函数之间：2 行空行
- 函数内逻辑块之间：1 行空行
- 类的 public/protected/private 之间：1 行空行

```cpp
class PageAllocator {
public:
    void* alloc();

    void free(void* ptr);


private:
    uint64_t* bitmap_;

    uint64_t count_;
};
```

### 4.3 指针与引用

```cpp
// 左对齐风格
void* alloc_page();
const char* name;

// 或者
void *alloc_page();
const char *name;

// **禁止混用**
void*  ptr1;   // ✅
void *ptr2;    // ✅
void * ptr3;   // ❌
```

---

## 五、现代 C++ 使用指南

### 5.1 允许的特性

```cpp
// constexpr / consteval：编译期计算
constexpr uint64_t kPageSize = 4096;
consteval int square(int n) { return n * n; }

// 模板和类型推导
template<typename T>
constexpr T max(T a, T b) { return a > b ? a : b; }

// concepts / requires（C++20）
template<typename T>
concept Integral = std::is_integral_v<T>;

template<Integral T>
T add(T a, T b) { return a + b; }

// RAII
class LockGuard {
    Spinlock& lock_;
public:
    explicit LockGuard(Spinlock& lock) : lock_(lock) { lock.acquire(); }
    ~LockGuard() { lock_.release(); }
};

// lambda
auto task = []() { kprintf("Hello from task\n"); };

// [[nodiscard]]：禁止忽略返回值
[[nodiscard]] void* alloc_page();

// [[noreturn]]：函数不会返回
[[noreturn]] void panic(const char* msg);
```

### 5.2 禁止的特性

```cpp
// ❌ 禁止异常
try { } catch (...) { }

// ❌ 禁止 RTTI
dynamic_cast<Type*>(ptr);
typeid(obj);

// ❌ 禁止标准库
#include <vector>
#include <string>
std::vector<int> v;

// ✅ 替代方案：自实现
#include "kernel/lib/vector.hpp"
Vector<int> v;
```

---

## 六、CMake 规范

### 6.1 命名风格

```cmake
# 变量：大写下划线
set(CMAKE_CXX_FLAGS_INIT "-std=c++23")
set(BUILD_DIR "${CMAKE_SOURCE_DIR}/build")

# 目标：小写下划线
add_executable(cinux_kernel ...)
add_library(libc STATIC ...)

# 宏：大写下划线
add_definitions(-DCINUX_DEBUG)
```

### 6.2 注释风格

```cmake
# ============================================================
# 区域标题
# ============================================================

# 单行注释：说明本行作用
add_subdirectory(kernel)  # 添加内核模块

# TODO: 说明待完成事项
# TODO: 添加测试目标
```

---

## 七、检查清单

提交代码前，请确认：

- [ ] 所有函数/类有 Doxygen 风格注释
- [ ] 变量命名符合规范（snake_case / kPascalCase / UPPER_CASE）
- [ ] 代码缩进使用 4 空格
- [ ] 未使用异常和 RTTI
- [ ] 未包含 `<iostream>` 等标准库
- [ ] 汇编指令右侧有语义注释
- [ ] TODO 注释包含实现思路

---

> **最后更新**：Milestone 000_env_toolchain
> **维护者**：Cinux 项目组
