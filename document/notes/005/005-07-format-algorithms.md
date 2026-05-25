---
title: 格式化算法
---

# 005-07: 格式化算法详解

## 概述

Cinux 的格式化函数提供数字到字符串的转换功能，支持十进制、十六进制和二进制格式。这些函数设计精简，针对内核环境优化，避免了标准库的依赖。

## 函数接口

文件：[`kernel/mini/lib/private/format.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/private/format.h)

```cpp
namespace cinux::mini::lib::detail {

int format_decimal(int64_t value, char* buffer, int buffer_size);
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase);
int format_binary(uint64_t value, char* buffer, int buffer_size);

}
```

---

## 1. format_decimal - 十进制格式化

### 算法流程

```
输入: value = -12345
      buffer[64]

1. 判断符号
   is_neg = (value < 0) = true
   value = -value = 12345

2. 反向提取数字（低位到高位）
   tmp[0] = '5'  (12345 % 10)
   tmp[1] = '4'  (1234 % 10)
   tmp[2] = '3'  (123 % 10)
   tmp[3] = '2'  (12 % 10)
   tmp[4] = '1'  (1 % 10)

3. 写入符号
   buffer[0] = '-'

4. 反向复制到输出
   buffer = "-", tmp 反向
   buffer = "-12345"

输出: "-12345", len = 6
```

### 核心代码

```cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int idx = 0;
    bool is_neg = value < 0;

    // INT64_MIN 特殊处理（无法直接取负）
    if (is_neg) {
        if (value == INT64_MIN) {
            const char* min_str = "-9223372036854775808";
            int len = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1) {
                buffer[idx++] = min_str[len++];
            }
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    // 反向提取数字
    uint64_t abs_val = static_cast<uint64_t>(value);
    char tmp[24];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + (abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    // 添加符号
    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    // 反向复制
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

### 测试用例

| 输入 | 输出 | 长度 |
|------|------|------|
| `0` | `"0"` | 1 |
| `42` | `"42"` | 2 |
| `-12345` | `"-12345"` | 6 |
| `INT64_MAX` | `"9223372036854775807"` | 19 |
| `INT64_MIN` | `"-9223372036854775808"` | 20 |

---

## 2. format_hex - 十六进制格式化

### 算法流程

```
输入: value = 0xDEADBEEF
      lowercase = true

1. 提取半字节（从低位到高位）
   tmp[0] = 'f'  (0xEF & 0xF)
   tmp[1] = 'e'  (0xDE & 0xF)
   tmp[2] = 'e'  (0xAD & 0xF)
   tmp[3] = 'b'  (0xEA & 0xF)
   tmp[4] = 'd'  (0xD5 & 0xF)
   tmp[5] = 'a'  (0x6A & 0xF)
   tmp[6] = 'e'  (0x35 & 0xF)
   tmp[7] = 'd'  (0x1A & 0xF)

2. 反向复制
   buffer = "deadbeef"

输出: "deadbeef", len = 8
```

### 核心代码

```cpp
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char tmp[20];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

### 测试用例

| 输入 | lowercase | 输出 | 长度 |
|------|-----------|------|------|
| `0x0` | true | `"0"` | 1 |
| `0xA` | true | `"a"` | 1 |
| `0xA` | false | `"A"` | 1 |
| `0xDEADBEEF` | true | `"deadbeef"` | 8 |
| `0xDEADBEEF` | false | `"DEADBEEF"` | 8 |
| `0x123456789ABCDEF0` | true | `"123456789abcdef0"` | 16 |

---

## 3. format_binary - 二进制格式化

### 算法特点

- **压缩前导零**：自动去除高位无意义的 `0`
- **零值特殊处理**：输入 `0` 输出 `"0"`

### 算法流程

```
输入: value = 0b00101010

1. 找到最高位 1
   bit = 63, 62, ... 查找
   找到 bit = 5 是第一个 1

2. 从最高位开始输出
   buffer = "101010"

输出: "101010", len = 6
```

### 核心代码

```cpp
int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    // 找到最高位的 1
    int bit = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found) bit = 0;  // value = 0

    // 输出二进制字符串
    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';

    return idx;
}
```

### 测试用例

| 输入 | 输出 | 长度 |
|------|------|------|
| `0b0` | `"0"` | 1 |
| `0b1` | `"1"` | 1 |
| `0b101010` | `"101010"` | 6 |
| `0b00101` | `"101"` | 3（压缩前导零） |
| `0xFFFFFFFFFFFFFFFF` | 64 个 `"1"` | 64 |
| `1ULL << 63` | `"1000...000"` | 64 |

---

## 边界情况处理

### INT64_MIN 特殊处理

```cpp
// INT64_MIN = -9223372036854775808
// 无法用 int64_t 表示其绝对值（会溢出）
if (value == INT64_MIN) {
    const char* min_str = "-9223372036854775808";
    // 直接复制字符串
    ...
}
```

### 缓冲区保护

所有函数都检查 `buffer_size`，防止缓冲区溢出：

```cpp
if (buffer_size < 1) return 0;
while (idx < buffer_size - 1) { ... }
buffer[idx] = '\0';
```

---

## 性能特点

| 函数 | 时间复杂度 | 空间复杂度 |
|------|-----------|-----------|
| format_decimal | O(log₁₀ n) | O(log₁₀ n) |
| format_hex | O(log₁₆ n) | O(log₁₆ n) |
| format_binary | O(64) 最坏 | O(64) |

---

## 相关链接

- [005-06: kprintf 格式化实现](005-06-kprintf-format.md) - vkprintf_impl 模板设计
- [`kernel/mini/lib/private/format.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/private/format.cpp) - 完整实现
- [`test/unit/test_kprintf_format.cpp`](https://github.com/CinuxOS/Cinux/blob/main/test/unit/test_kprintf_format.cpp) - 单元测试
