# Phase 0-B: 代码优化审查（按模块）

> **决策确认**：严格串行在 0-A 之后。C++23 激进采用（concepts, expected, constexpr）。
> 接口可改但必须同步更新所有调用方。现有测试通过 + 补充缺失测试。
> 模块顺序：低依赖优先。mini kernel 不管。命名空间保持现状。
> 错误处理统一为 Expected 模式。BSD/Allman 代码风格不变。

## 优化原则

1. **激进 C++23**：concepts、std::expected、constexpr、enum class 全部可用
2. **接口可变**：公共 API 可以改签名，但必须同步更新所有调用方
3. **测试验收**：现有测试全部通过 + 对修改的模块补齐缺失测试
4. **错误处理**：内核内部统一用 `ErrorOr<T>` (std::expected<T, Error>) 替代裸 int/bool 返回值
5. **命名空间**：保持现有 cinux::module 风格不变

## 模块优化顺序

### M1: kernel/lib/（基础库） — 风险：低
**C-Style 问题**：C 风格字符串函数、printf va_list、extern "C" 链接

- [ ] 引入 constexpr 版本的字符串操作（strlen, strcmp → consteval/constexpr）
- [ ] kprintf 内部：评估 template variadic vs va_list（内核环境可能需保留 va_list）
- [ ] 增加边界检查的 safe 版本（strlcpy 替代 strcpy）
- [ ] extern "C" 对汇编接口保持不变
- [ ] 补充字符串函数的测试用例

### M2: kernel/mm/（内存管理） — 风险：中
**C-Style 问题**：Heap magic number、原始指针、位图操作

- [ ] Heap 块头改为 struct with constructor，去掉 magic number
- [ ] 引入 RAII wrapper for physical pages（PhysicalPage guard class，析构自动释放）
- [ ] 评估 Span<T> 替代 raw pointer + length
- [ ] PMM bitmap 操作改为 constexpr template functions
- [ ] VMM 接口返回值改为 ErrorOr<T>
- [ ] 补充 PMM/VMM 边界测试

### M3: kernel/fs/（文件系统） — 风险：中
**C-Style 问题**：void* buffer、手动 memcpy、巨型文件（已在 0-A 拆分）

- [ ] 文件拆分已在 0-A 完成，此处只做代码风格优化
- [ ] 引入强类型 Buffer wrapper 替代 void*
- [ ] ext2 disk layout struct 用 `[[gnu::packed]]` 或位域标注
- [ ] 路径操作使用自定义 StringView（零堆分配）
- [ ] VFS 层接口返回值改为 ErrorOr<T>
- [ ] 补充 ext2 边界条件测试

### M4: kernel/drivers/（驱动层） — 风险：中
**C-Style 问题**：volatile pointer MMIO、C-style enum、固定数组

- [ ] MMIO 寄存器封装为类型安全的 Reg<offset> 模板
- [ ] C-style enum → enum class + 操作符重载（Packet0, Ps2Cmd 等）
- [ ] 固定数组 → Array<T, N>（std::array 的内核替代）
- [ ] I/O port 操作封装为 inline template 函数
- [ ] 驱动接口保持不变（为 Phase 1 块设备抽象做准备）
- [ ] 补充各驱动的基本测试

### M5: kernel/proc/（进程管理） — 风险：高
**C-Style 问题**：固定数组 run_queue、原始指针管理 task、巨型文件（已在 0-A 拆分）

- [ ] 文件拆分已在 0-A 完成
- [ ] run_queue 改为侵入式链表（为 Phase 1 SMP 多核调度做准备）
- [ ] Process/TCB 数据结构 class 化，明确所有权
- [ ] TaskBuilder 增加验证逻辑，用 Expected 返回错误
- [ ] 引入 scope guard for context switch
- [ ] 补充进程生命周期测试

### M6: kernel/syscall/（系统调用） — 风险：低
**C-Style 问题**：char[] 路径处理、裸 int 返回值

- [ ] 路径操作统一用 StringView
- [ ] 返回值改为 ErrorOr<T>（内部），syscall 入口点保持 -errno 约定
- [ ] 统一 errno 映射（对齐 Linux errno 值）
- [ ] 补充 syscall 错误路径测试

### M7: kernel/ipc/（进程间通信） — 风险：低
**C-Style 问题**：手动 ring buffer index 管理、C 风格数组

- [ ] Ring buffer 模板化 RingBuffer<T, N>
- [ ] 管道接口返回值改为 ErrorOr
- [ ] 补充管道边界条件测试

### M8: kernel/gui/（图形界面） — 风险：中
**注意**：此模块将在 Phase 1-B 剥离为独立仓库，优化需考虑库分离方向

- [ ] 窗口管理改为更灵活的容器（为库分离做准备）
- [ ] 事件系统类型化
- [ ] Framebuffer 接口抽象化（IHardwareSurface）
- [ ] 补充 GUI 单元测试

### M9: kernel/arch/x86_64/（架构层） — 风险：高，最低优先级
**原则**：arch 层改动影响全局，只做安全优化

- [ ] 中断处理表评估 template 化可行性
- [ ] 内联汇编保持现状（不可避免）
- [ ] 只做常量命名和类型安全优化
- [ ] 不改动核心逻辑

## 产出物
- [ ] 每个模块的优化 patch（独立 commit，按任务提交）
- [ ] 优化前后测试对比
- [ ] 补充的测试用例
