# M1: 调试工具 — GDB Stub + KALLSYMS

> 内核远程调试能力。GDB remote protocol stub、内核符号表、
> panic 时打印 backtrace。

## 任务清单

### T1: GDB Remote Stub

**文件**: `kernel/debug/gdb_stub.hpp`, `kernel/debug/gdb_stub.cpp`

实现 GDB Remote Serial Protocol 的最小子集：

```cpp
class GDBStub {
public:
    void init();  // 通过 serial port COM1

    // 在断点/异常时进入
    void enter(InterruptFrame* frame);

    // 命令处理
    void process_command(const char* cmd);

    // 寄存器读写
    void read_registers(InterruptFrame* frame, char* out);
    void write_registers(InterruptFrame* frame, const char* data);

    // 内存读写
    void read_memory(uint64_t addr, size_t len, char* out);
    void write_memory(uint64_t addr, const char* data, size_t len);

    // 单步/继续
    void continue_exec();
    void single_step();

private:
    bool active_;
    Serial* serial_;
};
```

**支持的 GDB 命令**:
| 命令 | 说明 |
|------|------|
| g | 读寄存器 |
| G | 写寄存器 |
| m | 读内存 |
| M | 写内存 |
| s | 单步 |
| c | 继续 |
| Z0/Z1 | 设置软件/硬件断点 |
| z0/z1 | 清除断点 |
| ? | 查询停止原因 |

- [ ] GDB remote protocol 包解析（$packet#checksum）
- [ ] 寄存器读写（x86_64 16 个通用寄存器）
- [ ] 内存读写
- [ ] 单步（RFLAGS.TF trap flag）
- [ ] 断点管理（INT3 软件断点）
- [ ] 串口通信层

### T2: KALLSYMS（内核符号表）

**文件**: `kernel/debug/kallsyms.hpp`, `kernel/debug/kallsyms.cpp`

编译时收集内核符号，嵌入到内核二进制中：

```cpp
struct KSymbol {
    uint64_t address;
    uint8_t  type;      // 'T' text, 'D' data, 'B' bss, etc.
    char     name[64];
};

class KAllSyms {
public:
    void init();  // 从嵌入的符号表初始化

    // 地址 → 符号名
    const char* find_symbol(uint64_t addr, uint64_t* offset);

    // 打印 backtrace
    void print_backtrace(uint64_t rbp);
};
```

- [ ] 构建脚本：`nm kernel.elf | sort > kallsyms.bin`
- [ ] 链接器嵌入符号表
- [ ] init() 解析嵌入数据
- [ ] find_symbol() 二分查找
- [ ] print_backtrace() 栈帧遍历

### T3: Panic 增强

**文件**: `kernel/arch/x86_64/exception_handlers.cpp`

- [ ] panic 时打印：
  - 寄存器 dump（RAX-R15, RIP, RSP, RFLAGS, CR2）
  - backtrace（函数名 + 偏移）
  - 当前 task 信息
- [ ] 可选：进入 GDB stub（等待远程调试器连接）

### T4: dmesg 增强

- [ ] panic 日志自动保存到 kernel log buffer
- [ ] 重启后可通过 dmesg 查看上次 panic 信息

## 产出物

- [ ] `kernel/debug/gdb_stub.hpp` / `.cpp` — GDB remote stub
- [ ] `kernel/debug/kallsyms.hpp` / `.cpp` — 内核符号表
- [ ] 构建脚本符号表嵌入
- [ ] Panic backtrace 增强
- [ ] `make debug` 目标：QEMU + GDB 连接
