# M0: Core Type Library

> F1 的前置基础。为所有后续 Milestone 和 Feature 域提供零 OS 耦合的通用类型库。
> 所有组件均为 header-only constexpr 实现，无动态内存分配，无系统调用依赖。

## 目标

在 `kernel/lib/` 下实现 5 个核心类型，替代内核中的 C 风格原始类型组合（`T* + length`、`void* + size`、裸 int 错误码）。

## 消费方映射

| 组件 | 消费方 |
|------|--------|
| ErrorOr\<T\> | M2 日志返回值 / F2 VMM·PMM 接口 / F6 VFS 层 / F8 Pipe 接口 / F10 syscall 内部 |
| StringView | F6 路径解析 / F10 syscall 路径参数 / M2 日志格式化 |
| Span\<T\> | M1 RingBuffer 批量操作 / M3 DMA scatter-gather / F2 物理页操作 |
| Buffer | M3 DmaBuffer 替代 / F6 ext2 块读写 / F7 网络包 payload |
| Array\<T, N\> | M1 RingBuffer 内部存储 / F5 驱动固定寄存器组 / F4 per-CPU 数据 |

## 任务清单

### T1: ErrorOr\<T\> — 通用错误处理

**文件**: `kernel/lib/expected.hpp`

```cpp
namespace cinux::lib {

enum class Error : uint32_t {
    Ok = 0,
    OutOfMemory,
    InvalidArgument,
    NotFound,
    IOError,
    AlreadyExists,
    PermissionDenied,
    WouldBlock,
    BufferOverflow,
    NotImplemented,
};

template <typename T>
class ErrorOr {
public:
    // 成功路径
    constexpr ErrorOr(T value);
    // 错误路径
    constexpr ErrorOr(Error err);

    constexpr bool ok() const;
    constexpr explicit operator bool() const;

    constexpr T& value();
    constexpr const T& value() const;
    constexpr Error error() const;

private:
    union {
        T value_;
        Error error_;
    };
    bool is_ok_;
};

} // namespace cinux::lib
```

- [ ] `Error` 枚举类：通用错误码集合
- [ ] `ErrorOr<T>` 模板：value/error 判别式联合体
- [ ] `ok()` / `operator bool()` / `value()` / `error()` 方法
- [ ] 特化 `ErrorOr<void>`（无值，只判成功/失败）
- [ ] 全部 constexpr，无堆分配
- [ ] 断言保护：`value()` 在 error 状态调用触发 kernel panic

### T2: StringView — 零分配字符串视图

**文件**: `kernel/lib/string_view.hpp`

```cpp
namespace cinux::lib {

class StringView {
public:
    constexpr StringView() = default;
    constexpr StringView(const char* str);
    constexpr StringView(const char* str, size_t len);

    // 状态查询
    constexpr size_t size() const;
    constexpr bool empty() const;
    constexpr const char* data() const;

    // 元素访问
    constexpr char operator[](size_t i) const;
    constexpr char front() const;
    constexpr char back() const;

    // 比较
    constexpr bool equals(StringView other) const;
    constexpr bool starts_with(StringView prefix) const;
    constexpr bool ends_with(StringView suffix) const;

    // 查找
    constexpr size_t find(char c, size_t pos = 0) const;
    constexpr size_t find(StringView needle, size_t pos = 0) const;
    constexpr size_t rfind(char c) const;

    // 子串
    constexpr StringView substr(size_t pos, size_t count = npos) const;

    // 比较
    constexpr int compare(StringView other) const;
    constexpr bool operator==(StringView other) const;
    constexpr bool operator!=(StringView other) const;
    constexpr bool operator<(StringView other) const;

    static constexpr size_t npos = static_cast<size_t>(-1);

private:
    const char* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace cinux::lib
```

- [ ] 构造函数：C 字符串 + 显式长度
- [ ] 比较：equals / starts_with / ends_with / compare
- [ ] 查找：find / rfind（单字符 + 子串）
- [ ] 子串：substr
- [ ] 运算符：== / != / <（支持 constexpr switch/case）
- [ ] 全部 constexpr
- [ ] 无 `\0` 终止假设，纯长度语义

### T3: Span\<T\> — 非拥有连续内存视图

**文件**: `kernel/lib/span.hpp`

```cpp
namespace cinux::lib {

template <typename T>
class Span {
public:
    constexpr Span() = default;
    constexpr Span(T* data, size_t size);
    constexpr Span(T* begin, T* end);

    template <size_t N>
    constexpr Span(T (&arr)[N]);

    // 状态查询
    constexpr size_t size() const;
    constexpr bool empty() const;
    constexpr T* data() const;

    // 元素访问
    constexpr T& operator[](size_t i) const;
    constexpr T& front() const;
    constexpr T& back() const;

    // 子视图
    constexpr Span first(size_t count) const;
    constexpr Span last(size_t count) const;
    constexpr Span subspan(size_t pos, size_t count = npos) const;

    // 迭代
    constexpr T* begin() const;
    constexpr T* end() const;

    static constexpr size_t npos = static_cast<size_t>(-1);

private:
    T* data_ = nullptr;
    size_t size_ = 0;
};

// 常用别名
using ByteSpan = Span<uint8_t>;
using ConstByteSpan = Span<const uint8_t>;

} // namespace cinux::lib
```

- [ ] 构造函数：指针+长度 / 指针对 / C 数组
- [ ] 子视图：first / last / subspan
- [ ] 迭代器：begin / end（支持 range-for）
- [ ] 类型别名：ByteSpan / ConstByteSpan
- [ ] 全部 constexpr
- [ ] `const T` 特化确保只读语义

### T4: Buffer — 类型安全字节缓冲区

**文件**: `kernel/lib/buffer.hpp`

```cpp
namespace cinux::lib {

// 非拥有视图
class BufferView {
public:
    constexpr BufferView() = default;
    constexpr BufferView(const void* data, size_t size);

    constexpr const uint8_t* data() const;
    constexpr size_t size() const;
    constexpr bool empty() const;

    constexpr BufferView slice(size_t offset, size_t len) const;
    constexpr const uint8_t& operator[](size_t i) const;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// 拥有型缓冲区（栈上固定大小）
template <size_t N>
class StaticBuffer {
public:
    constexpr StaticBuffer() = default;

    constexpr uint8_t* data();
    constexpr const uint8_t* data() const;
    constexpr size_t capacity() const;
    constexpr size_t size() const;
    constexpr void resize(size_t new_size);

    constexpr void fill(uint8_t value);
    constexpr void copy_from(const void* src, size_t len);
    constexpr void copy_to(void* dst, size_t len) const;

    constexpr BufferView view() const;
    constexpr ByteSpan as_span();

private:
    uint8_t data_[N]{};
    size_t size_ = 0;
};

} // namespace cinux::lib
```

- [ ] `BufferView`：非拥有只读字节视图
- [ ] `StaticBuffer<N>`：固定大小拥有型缓冲区
- [ ] slice / fill / copy_from / copy_to 操作
- [ ] `view()` / `as_span()` 桥接到 Span\<T\>
- [ ] 全部 constexpr，无堆分配

### T5: Array\<T, N\> — 固定大小容器

**文件**: `kernel/lib/array.hpp`

```cpp
namespace cinux::lib {

template <typename T, size_t N>
class Array {
public:
    // 元素访问
    constexpr T& operator[](size_t i);
    constexpr const T& operator[](size_t i) const;
    constexpr T& front();
    constexpr const T& front() const;
    constexpr T& back();
    constexpr const T& back() const;
    constexpr T* data();
    constexpr const T* data() const;

    // 状态查询
    constexpr size_t size() const;
    constexpr bool empty() const;

    // 迭代
    constexpr T* begin();
    constexpr const T* begin() const;
    constexpr T* end();
    constexpr const T* end() const;

    // 操作
    constexpr void fill(const T& value);

    // 比较
    constexpr bool operator==(const Array& other) const;
    constexpr bool operator!=(const Array& other) const;

private:
    T data_[N]{};
};

} // namespace cinux::lib
```

- [ ] 元素访问 + 边界检查（debug build 断言）
- [ ] 迭代器支持（range-for）
- [ ] `fill()` 批量填充
- [ ] 相等比较运算符
- [ ] 全部 constexpr
- [ ] 聚合体初始化支持 `Array<int, 3> a = {1, 2, 3}`

### T6: 单元测试

**文件**: `kernel/test/test_core_types.cpp`

- [ ] ErrorOr\<T\>：成功/错误路径、value() 访问、ErrorOr\<void\> 特化
- [ ] StringView：构造、比较、查找、子串、空视图边界
- [ ] Span\<T\>：构造（含 C 数组）、子视图、迭代、空 span
- [ ] BufferView / StaticBuffer：slice、fill、copy、桥接 Span
- [ ] Array\<T, N\>：下标访问、fill、比较、聚合初始化
- [ ] 跨类型互操作：Buffer → Span、Array → Span、StringView 与 C 字符串互转

## 产出物

- [ ] `kernel/lib/expected.hpp` — ErrorOr\<T\> + Error 枚举
- [ ] `kernel/lib/string_view.hpp` — StringView
- [ ] `kernel/lib/span.hpp` — Span\<T\> + ByteSpan 别名
- [ ] `kernel/lib/buffer.hpp` — BufferView + StaticBuffer\<N\>
- [ ] `kernel/lib/array.hpp` — Array\<T, N\>
- [ ] `kernel/test/test_core_types.cpp` — 单元测试
- [ ] 编译通过 + 测试通过
