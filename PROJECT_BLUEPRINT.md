# Cinux 项目蓝图

> 本文档是 Cinux 仓库的顶层设计文档，供开发者（也供本地 AI）快速理解项目全貌。

---

## 一、项目定位

| 维度 | 说明 |
|------|------|
| 目标 | 从零手搓可用的 x64 操作系统，最终实现 GUI |
| 工具链 | GNU AS（AT&T 语法）+ GCC/G++ + CMake，完全 GNU |
| 语言 | C++（现代特性，禁用标准库/异常/RTTI）+ AT&T 汇编 |
| 引导 | 自写 Bootloader，real→protected→long mode 完整流程 |
| 测试 | 自研轻量测试框架，QEMU 串口断言 + host 端 mock 单元测试 |
| 教程 | 中文双轨：动手版（跟着做）+ 通读版（看完整代码） |
| 文档 | MkDocs 静态站点，自动部署 |

---

## 二、CMake 构建架构

```cmake
# 顶层目标概览
make run          # 构建完整镜像 + 启动 QEMU
make run-debug    # 启动 QEMU + GDB server（端口 1234）
make test         # 运行所有 host 端单元测试
make test-qemu    # 运行 QEMU 集成测试（需要 QEMU）
make docs         # 构建 MkDocs 文档
make clean        # 清理构建产物
```

构建产物：
- `build/cinux.img`：可启动磁盘镜像（Bootloader + 内核 + initrd）
- `build/kernel.elf`：内核 ELF（用于 GDB 符号调试）
- `build/test/*`：host 端测试二进制

---

## 三、Git Tag 规范

```
格式：编号_大主题_小阶段
示例：
  000_env_toolchain
  001_boot_real_mode
  005_kernel_entry
  011_mm_pmm
  019_syscall
  024_fs_ext2
```

**里程碑 tag 含义**：打上 tag 的时刻 = 该章节所有 checkbox 全部完成，代码可编译运行，教程已生成。

---

## 四、测试框架设计

### 宏接口

```cpp
// test/framework/test_framework.h

// 定义一个测试
TEST("测试名称") {
    // 测试体
}

// 断言宏
ASSERT_EQ(actual, expected)     // 相等
ASSERT_NE(actual, expected)     // 不等
ASSERT_TRUE(expr)               // 为真
ASSERT_FALSE(expr)              // 为假
ASSERT_NULL(ptr)                // 为空
ASSERT_NOT_NULL(ptr)            // 非空
ASSERT_GE(a, b)                 // a >= b
ASSERT_LE(a, b)                 // a <= b

// QEMU 端专用
QTEST("测试名称") {
    // 结果通过 serial_write("[PASS]/[FAIL] ...") 输出
}
```

### Host 编译约束

Host 端测试通过 `-DCINUX_HOST_TEST` 宏启用 mock 路径：

```cpp
#ifdef CINUX_HOST_TEST
// 使用 mock 实现
#else
// 使用真实硬件实现
#endif
```

---

## 六、现代 C++ 使用规范

| 特性 | 允许 | 典型用途 |
|------|------|---------|
| `constexpr` / `consteval` | ✅ | 编译期计算 GDT/IDT 描述符、端口地址 |
| 模板元编程 | ✅ | 类型安全的寄存器抽象、编译期位操作 |
| `concepts` / `requires` | ✅ | 约束文件系统接口、驱动接口 |
| RAII | ✅ | 锁、内存资源、文件描述符的生命周期 |
| 智能指针 | ✅（自实现）| `UniquePtr<T>`、`SharedPtr<T>` 内核版本 |
| lambda | ✅ | 中断回调、调度钩子 |
| `std::` 标准库 | ❌ | 全部自实现（`kernel/lib/`） |
| 异常 (`try/catch`) | ❌ | 用返回值 / `Result<T,E>` 替代 |
| RTTI (`dynamic_cast`) | ❌ | 用虚函数表 + 手写类型标签替代 |

---

## 七、QEMU 调试速查

```bash
# 基础启动
qemu-system-x86_64 -drive file=build/cinux.img,format=raw \
    -serial stdio -m 256M -no-reboot -no-shutdown

# GDB 调试
qemu-system-x86_64 ... -s -S  # -s: GDB 端口 1234; -S: 启动后暂停
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue

# 查看串口输出（保存到文件）
qemu-system-x86_64 ... -serial file:serial.log

# 开启 QEMU monitor
qemu-system-x86_64 ... -monitor stdio
# monitor 命令：info registers, info mem, x/10x 0x100000
```
